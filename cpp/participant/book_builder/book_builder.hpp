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

 private:
  using LevelMap = std::map<std::int64_t, std::int64_t>;  // price -> aggregate quantity

  [[nodiscard]] LevelMap& levels_for(Side side) { return side == Side::kBuy ? bids_ : asks_; }
  [[nodiscard]] const LevelMap& levels_for(Side side) const {
    return side == Side::kBuy ? bids_ : asks_;
  }

  /// Adds `delta_units` (positive or negative) to the level at `price_units`,
  /// erasing the level outright if it would reach zero or below.
  void adjust_level(Side side, std::int64_t price_units, std::int64_t delta_units);
  void set_level(Side side, std::int64_t price_units, std::int64_t quantity_units);

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
};

}  // namespace aegis::participant::book
