#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cpp/exchange/order_book/level_index.hpp"
#include "cpp/exchange/order_book/order_storage.hpp"
#include "cpp/exchange/order_book/types.hpp"

/// The central limit order book for one instrument (AEGIS-027, AEGIS-030,
/// AEGIS-036, AEGIS-037, AEGIS-038).
///
/// Single instrument, single writer, no statics. `OrderBook` knows how to
/// store, order and remove resting orders; it has no knowledge of matching,
/// validation or wire messages — `cpp-exchange-matching` depends on this
/// layer, never the reverse (configs/architecture_rules.yaml).
namespace aegis::exchange {

class OrderBook {
 public:
  explicit OrderBook(InstrumentId instrument_id) : instrument_id_(instrument_id) {}

  /// Reserves slab and order-index capacity ahead of the steady-state
  /// zero-allocation cycle (AEGIS-037; slice 6 adds the counters that measure
  /// it against an injected `memory_resource`).
  void reserve(std::size_t order_capacity);

  /// Inserts `descriptor` as a resting order at its price, at the tail of
  /// that level's FIFO queue — price-time priority by construction
  /// (AEGIS-027). `descriptor.prev_index`/`next_index` are ignored and
  /// overwritten.
  ///
  /// Precondition: `descriptor.order_id` is not already live in this book.
  void add(const OrderNode& descriptor);

  /// Removes the order by id: one hash lookup plus one O(1) queue unlink —
  /// no price-queue scan (AEGIS-030). Returns the order's state immediately
  /// before removal, or `std::nullopt` if `order_id` is not live.
  std::optional<OrderNode> cancel(OrderId order_id);

  [[nodiscard]] const OrderNode* find(OrderId order_id) const;
  [[nodiscard]] OrderNode* find(OrderId order_id);

  /// The order id currently live under `(participant_id, client_order_id)`,
  /// if any. Scope is per-participant, not global (ADR-0011).
  [[nodiscard]] std::optional<OrderId> find_live_by_client_id(ParticipantId participant_id,
                                                              ClientOrderId client_order_id) const;

  [[nodiscard]] std::optional<PriceUnits> best_bid() const { return bids_.best_price(); }
  [[nodiscard]] std::optional<PriceUnits> best_ask() const { return asks_.best_price(); }

  [[nodiscard]] const PriceLevel* level_at(Side side, PriceUnits price) const;

  /// Order ids at `price` on `side`, oldest first — the FIFO arrival order
  /// golden sequences check (AEGIS-027).
  [[nodiscard]] std::vector<OrderId> orders_at(Side side, PriceUnits price) const;

  [[nodiscard]] InstrumentId instrument_id() const { return instrument_id_; }
  [[nodiscard]] std::size_t live_order_count() const { return storage_.live_count(); }

  [[nodiscard]] const OrderStorage& storage() const { return storage_; }
  [[nodiscard]] const LevelIndex& levels(Side side) const { return levels_for(side); }

 private:
  [[nodiscard]] LevelIndex& levels_for(Side side) {
    return side == Side::kBuy ? static_cast<LevelIndex&>(bids_) : static_cast<LevelIndex&>(asks_);
  }
  [[nodiscard]] const LevelIndex& levels_for(Side side) const {
    return side == Side::kBuy ? static_cast<const LevelIndex&>(bids_)
                              : static_cast<const LevelIndex&>(asks_);
  }

  InstrumentId instrument_id_;
  OrderStorage storage_;
  MapLevelIndex bids_{/*descending=*/true};
  MapLevelIndex asks_{/*descending=*/false};
  std::unordered_map<std::uint64_t, std::size_t> order_index_;  ///< OrderId::value() -> slab index.
  std::map<std::pair<std::uint64_t, std::uint64_t>, OrderId>
      live_client_ids_;  ///< (participant, client) -> OrderId.
};

}  // namespace aegis::exchange
