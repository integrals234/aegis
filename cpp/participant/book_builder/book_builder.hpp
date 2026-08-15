#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/events/exchange_messages.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/participant/feed_handler/sequence_tracker.hpp"

/// Participant-side book reconstruction (AEGIS-064, AEGIS-065, AEGIS-066,
/// AEGIS-067, AEGIS-069, AEGIS-070; ADR-0021).
///
/// `BookBuilder` maintains its own aggregated price-level state — it does not
/// depend on `cpp-exchange-order-book` (`configs/architecture_rules.yaml`'s
/// `cpp-participant-book-builder.may_depend_on` is `[cpp-common, cpp-events,
/// cpp-participant-feed-handler]` only), so it cannot reuse `OrderBook`'s
/// `LevelIndex`. One type serves both reconstruction modes: an
/// order-level map is populated only for entries/deltas that carry a nonzero
/// `order_id` (AEGIS-066); a feed that never supplies one drives the
/// aggregated levels alone via `DeltaKind::kPriceLevelSet` (AEGIS-067). A
/// session commits to one mode or the other; this type does not need to be
/// told which.
namespace aegis::participant::book {

using Side = events::exchange::Side;

struct PriceLevelView {
  std::int64_t price_units{0};
  std::int64_t quantity_units{0};

  friend bool operator==(const PriceLevelView&, const PriceLevelView&) = default;
};

struct OrderView {
  Side side{Side::kBuy};
  std::int64_t price_units{0};
  std::int64_t quantity_units{0};

  friend bool operator==(const OrderView&, const OrderView&) = default;
};

/// AEGIS-071. `spread_units`/`mid_price_units` are engaged only when both
/// sides are present; there is no meaningful spread or mid against an empty
/// side of the book.
struct TopOfBook {
  std::optional<PriceLevelView> best_bid;
  std::optional<PriceLevelView> best_ask;
  std::optional<std::int64_t> spread_units;
  std::optional<double> mid_price_units;
};

/// AEGIS-075. `original_quantity_units` is the largest quantity this level
/// has held since it last stood empty (its "high-water mark"); depletion is
/// only queryable while the level still exists — once it empties out
/// entirely, the level (and its history) is simply gone, matching how a
/// real book has no memory of a vacated price.
struct QueueDepletionSignal {
  std::int64_t original_quantity_units{0};
  std::int64_t current_quantity_units{0};
  /// `1.0 - current/original`, in `[0, 1)`: 0 means untouched since it was
  /// last at its peak, approaching 1 means nearly consumed.
  double depletion_ratio{0.0};
};

/// AEGIS-075. `fill_side` is the side of the *resting* order that was
/// filled — a filled bid (someone sold into it) is judged against the best
/// bid once the window elapses, a filled ask against the best ask.
/// `adverse` is true when that side's best price moved against the direction
/// beneficial to the side that was filled: down after a bid fill, up after
/// an ask fill — the classic "picked off right before a move" signal for a
/// resting order.
struct AdverseSelectionOutcome {
  Side fill_side{Side::kBuy};
  std::int64_t fill_price_units{0};
  /// The relevant side's best price when the window elapsed, if the book
  /// still had one; `std::nullopt` if that side was empty at evaluation.
  std::optional<std::int64_t> mark_price_units;
  bool adverse{false};
};

class BookBuilder {
 public:
  explicit BookBuilder(std::uint32_t instrument_id) : instrument_id_(instrument_id) {}

  [[nodiscard]] std::uint32_t instrument_id() const { return instrument_id_; }

  /// Discards all state and rebuilds it entirely from `snapshot` (AEGIS-064).
  /// Also the recovery step of AEGIS-070/AEGIS-061: if a recovery is in
  /// progress (`begin_recovery()`), this call re-bases the book on `snapshot`
  /// and then replays every buffered delta whose `md_sequence` is strictly
  /// greater than `snapshot.md_sequence` — the ones a gap-free feed would
  /// still have delivered after this snapshot — discarding the rest as
  /// already covered, and ends the recovery.
  /// `received_at_nanos` records when this message arrived, for AEGIS-069's
  /// staleness clock; 0 (the default) is fine for a caller that never
  /// configures staleness.
  /// Precondition: `snapshot.instrument_id == instrument_id()`.
  void apply_snapshot(const events::market_data::BookSnapshotEvent& snapshot,
                      common::Nanos received_at_nanos = 0);

  /// Applies one incremental change (AEGIS-065). `kOrderAdded`/
  /// `kOrderModified`/`kOrderRemoved` update the order-level map and adjust
  /// the aggregated level by the resulting delta (AEGIS-066); `kPriceLevelSet`
  /// sets the aggregated level directly, `order_id` unused (AEGIS-067). A
  /// delta naming an unknown `order_id` for `kOrderModified`/`kOrderRemoved`
  /// is ignored — applying it would fabricate state this builder never
  /// actually observed. While a recovery is in progress (`begin_recovery()`),
  /// `delta` is buffered rather than applied — see `apply_snapshot`.
  /// Precondition: `delta.instrument_id == instrument_id()`.
  void apply_delta(const events::market_data::BookDeltaEvent& delta,
                   common::Nanos received_at_nanos = 0);

  [[nodiscard]] std::optional<PriceLevelView> best(Side side) const;
  [[nodiscard]] std::optional<std::int64_t> quantity_at(Side side, std::int64_t price_units) const;

  /// Up to `depth` levels from the best price outward, best first.
  [[nodiscard]] std::vector<PriceLevelView> levels(Side side, std::size_t depth) const;

  /// Present only if this session has supplied order-level detail
  /// (AEGIS-066); `std::nullopt` for an order this builder has not tracked,
  /// or under a purely aggregated (AEGIS-067) session.
  [[nodiscard]] std::optional<OrderView> order(std::uint64_t order_id) const;

  [[nodiscard]] std::uint64_t last_md_sequence() const { return last_md_sequence_; }

  // ------------------------------------------------------- AEGIS-069, ADR-0021
  //
  // Staleness is a feed/book-level fact, never a risk decision (ADR-0021):
  // this class only answers `is_stale`. It is the caller's job to treat a
  // stale book's `best()`/`levels()` output as untrustworthy -- the
  // AEGIS-060 "stale-data response" -- rather than this class refusing to
  // answer at all, which would make the state unobservable exactly when a
  // caller most needs to inspect it.

  /// Both thresholds disabled (never stale) until called. `max_age` compares
  /// against the gap between the most recent `received_at_nanos` and the
  /// `now` passed to `is_stale`; `max_consecutive_faults` compares against
  /// `note_sequence_diagnostic`'s running count. Either alone is sufficient
  /// to declare staleness.
  void configure_staleness(common::Duration max_age, std::uint32_t max_consecutive_faults);

  /// Records the wall-clock time a message was received, independent of
  /// applying it — called on every message, including ones later found
  /// malformed, since the feed *was* heard from even if this instrument's
  /// specific record could not be used.
  void note_message_received(common::Nanos received_at_nanos);

  /// Feeds a sequence diagnostic (`cpp/participant/feed_handler/sequence_tracker.hpp`)
  /// into the consecutive-fault count: `kOk` resets it to 0, anything else
  /// increments it.
  void note_sequence_diagnostic(feed::SequenceDiagnostic diagnostic);

  [[nodiscard]] bool is_stale(common::Nanos now_nanos) const;

  // ----------------------------------------------------- AEGIS-070/061, ADR-0021

  /// Starts buffering incoming deltas instead of applying them, because the
  /// caller has detected a gap or reset it cannot trust this book to absorb
  /// safely. Ends when `apply_snapshot` next runs. Idempotent: calling it
  /// again while already recovering does not clear what has been buffered
  /// so far.
  void begin_recovery();
  [[nodiscard]] bool is_recovering() const { return recovering_; }

  // ---------------------------------------------------- AEGIS-071, AEGIS-072

  /// Best bid, best ask, and the spread/mid derived from them when both
  /// sides are present.
  [[nodiscard]] TopOfBook top_of_book() const;

  /// The quantity-weighted microprice: `(bid_qty * ask_price + ask_qty *
  /// bid_price) / (bid_qty + ask_qty)` — larger size on one side pulls the
  /// price toward the *other* side, reflecting the pressure that size
  /// represents. `std::nullopt` if either side is empty (defined edge case).
  [[nodiscard]] std::optional<double> microprice() const;

  // ------------------------------------------------------------- AEGIS-073

  /// `(bid_depth - ask_depth) / (bid_depth + ask_depth)` summed over the
  /// best `depth` levels per side, in `[-1, 1]`. `std::nullopt` if `depth ==
  /// 0` or both sides are empty over that depth (defined edge cases).
  [[nodiscard]] std::optional<double> depth_imbalance(std::size_t depth) const;

  // ------------------------------------------------------------- AEGIS-075

  /// `std::nullopt` if no level currently exists at `price_units` on `side`.
  [[nodiscard]] std::optional<QueueDepletionSignal> queue_depletion(Side side,
                                                                    std::int64_t price_units) const;

  /// Records a fill of a resting order on `fill_side` at `price_units`, to
  /// be judged after `window_updates` further `apply_delta`/`apply_snapshot`
  /// calls (each counts as one tick, whether or not it changes this specific
  /// level) — `drain_resolved_adverse_selection()` returns it once that many
  /// ticks have elapsed.
  void record_fill_for_adverse_selection(Side fill_side, std::int64_t price_units,
                                         std::uint32_t window_updates);

  /// Every fill whose window has fully elapsed as of the most recent
  /// `apply_delta`/`apply_snapshot` call, removing them from the pending set
  /// — each is returned exactly once.
  [[nodiscard]] std::vector<AdverseSelectionOutcome> drain_resolved_adverse_selection();

 private:
  using LevelMap = std::map<std::int64_t, std::int64_t>;  // price -> aggregate quantity

  [[nodiscard]] LevelMap& levels_for(Side side) { return side == Side::kBuy ? bids_ : asks_; }
  [[nodiscard]] const LevelMap& levels_for(Side side) const {
    return side == Side::kBuy ? bids_ : asks_;
  }

  /// Adds `delta_units` (positive or negative) to the level at `price_units`,
  /// erasing the level outright if it would reach zero or below. Also
  /// maintains that level's AEGIS-075 high-water mark: raised to the new
  /// quantity if higher, erased alongside the level.
  void adjust_level(Side side, std::int64_t price_units, std::int64_t delta_units);
  void set_level(Side side, std::int64_t price_units, std::int64_t quantity_units);
  void touch_watermark(Side side, std::int64_t price_units, std::int64_t new_quantity);

  [[nodiscard]] LevelMap& watermarks_for(Side side) {
    return side == Side::kBuy ? bid_watermarks_ : ask_watermarks_;
  }

  /// Advances every pending adverse-selection fill's countdown by one tick —
  /// called once per `apply_delta`/`apply_snapshot`.
  void tick_adverse_selection();

  std::uint32_t instrument_id_;
  std::uint64_t last_md_sequence_{0};
  // Bids keyed ascending but read highest-first via reverse iteration; asks
  // keyed ascending and read lowest-first via forward iteration -- one
  // comparator, no custom ordering functor needed for either side.
  LevelMap bids_;
  LevelMap asks_;
  std::unordered_map<std::uint64_t, OrderView> orders_;

  std::optional<common::Nanos> last_received_nanos_;
  std::optional<common::Duration> max_staleness_age_;
  std::uint32_t max_consecutive_faults_{0};  // 0 means this criterion is disabled.
  std::uint32_t consecutive_faults_{0};

  bool recovering_{false};
  std::vector<events::market_data::BookDeltaEvent> buffered_deltas_;

  // AEGIS-075: queue depletion.
  LevelMap bid_watermarks_;
  LevelMap ask_watermarks_;

  // AEGIS-075: post-fill adverse selection.
  struct PendingAdverseSelectionFill {
    Side side{Side::kBuy};
    std::int64_t price_units{0};
    std::uint32_t ticks_remaining{0};
  };
  std::vector<PendingAdverseSelectionFill> pending_adverse_selection_;
};

}  // namespace aegis::participant::book
