#pragma once

#include <cstddef>
#include <vector>

#include "cpp/exchange/order_book/level.hpp"
#include "cpp/exchange/order_book/types.hpp"

namespace aegis::exchange {

/// One resting or in-flight order's full state. Used both as `OrderBook`'s
/// internal storage record and as the descriptor callers pass to
/// `OrderBook::add` — `prev_index`/`next_index` are book-owned and overwritten
/// on insertion regardless of what the caller sets.
struct OrderNode {
  OrderId order_id;
  InstrumentId instrument_id;
  ParticipantId participant_id;
  ClientOrderId client_order_id;
  Side side{Side::kBuy};
  OrderType order_type{OrderType::kLimit};
  PriceUnits price_units;

  /// Fixed at creation; unchanged by fills, decreases, or cancellation
  /// (P1, ADR-0011 §4.10). A cancel-replace's replacement order has its own,
  /// separate `original_quantity`.
  QuantityUnits original_quantity;
  QuantityUnits cumulative_filled;
  QuantityUnits cancelled_quantity;
  QuantityUnits remaining;

  Priority priority;

  /// Intrusive doubly linked FIFO queue within one `PriceLevel`.
  std::size_t prev_index{kInvalidIndex};
  std::size_t next_index{kInvalidIndex};
};

/// Index-handle slab: nodes live in a vector at a stable index for their
/// lifetime; a free list of indices — never pointers — tracks reusable slots,
/// so growth cannot dangle a handle held elsewhere (AEGIS-037, ADR-0010).
///
/// `reserve()` up front is what lets a steady-state add/cancel/match cycle
/// perform zero allocations once warmed; slice 6 adds the injected
/// `memory_resource` and the counters that turn that into a measured
/// property rather than a design intention.
///
/// Deliberately not the M8 pool: no generation counters, no cross-book
/// sharing, no zero-allocation latency claim (AEGIS-042/043 own those).
class OrderStorage {
 public:
  void reserve(std::size_t capacity) { nodes_.reserve(capacity); }

  [[nodiscard]] std::size_t allocate() {
    if (!free_list_.empty()) {
      const auto index = free_list_.back();
      free_list_.pop_back();
      return index;
    }
    nodes_.emplace_back();
    return nodes_.size() - 1;
  }

  /// Precondition: `index` was returned by `allocate()` and not yet released.
  void release(std::size_t index) {
    nodes_[index] = OrderNode{};
    free_list_.push_back(index);
  }

  [[nodiscard]] OrderNode& node(std::size_t index) { return nodes_[index]; }
  [[nodiscard]] const OrderNode& node(std::size_t index) const { return nodes_[index]; }

  [[nodiscard]] std::size_t live_count() const { return nodes_.size() - free_list_.size(); }
  [[nodiscard]] std::size_t free_count() const { return free_list_.size(); }
  [[nodiscard]] std::size_t capacity() const { return nodes_.capacity(); }

 private:
  std::vector<OrderNode> nodes_;
  std::vector<std::size_t> free_list_;
};

}  // namespace aegis::exchange
