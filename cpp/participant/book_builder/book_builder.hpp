#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "cpp/events/exchange_messages.hpp"
#include "cpp/events/market_data_messages.hpp"

/// Participant-side book reconstruction (AEGIS-064, AEGIS-065, AEGIS-066,
/// AEGIS-067; ADR-0021).
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
  /// Precondition: `snapshot.instrument_id == instrument_id()`.
  void apply_snapshot(const events::market_data::BookSnapshotEvent& snapshot);

  /// Applies one incremental change (AEGIS-065). `kOrderAdded`/
  /// `kOrderModified`/`kOrderRemoved` update the order-level map and adjust
  /// the aggregated level by the resulting delta (AEGIS-066); `kPriceLevelSet`
  /// sets the aggregated level directly, `order_id` unused (AEGIS-067). A
  /// delta naming an unknown `order_id` for `kOrderModified`/`kOrderRemoved`
  /// is ignored — applying it would fabricate state this builder never
  /// actually observed.
  /// Precondition: `delta.instrument_id == instrument_id()`.
  void apply_delta(const events::market_data::BookDeltaEvent& delta);

  [[nodiscard]] std::optional<PriceLevelView> best(Side side) const;
  [[nodiscard]] std::optional<std::int64_t> quantity_at(Side side, std::int64_t price_units) const;

  /// Up to `depth` levels from the best price outward, best first.
  [[nodiscard]] std::vector<PriceLevelView> levels(Side side, std::size_t depth) const;

  /// Present only if this session has supplied order-level detail
  /// (AEGIS-066); `std::nullopt` for an order this builder has not tracked,
  /// or under a purely aggregated (AEGIS-067) session.
  [[nodiscard]] std::optional<OrderView> order(std::uint64_t order_id) const;

  [[nodiscard]] std::uint64_t last_md_sequence() const { return last_md_sequence_; }

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
};

}  // namespace aegis::participant::book
