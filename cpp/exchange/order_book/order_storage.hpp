#pragma once

#include <cstddef>
#include <memory_resource>
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

/// Index-handle slab: nodes live in a `std::pmr::vector` at a stable index for
/// their lifetime; a free list of indices — never pointers — tracks reusable
/// slots, so growth cannot dangle a handle held elsewhere (AEGIS-037,
/// ADR-0010).
///
/// Both containers take an injected `std::pmr::memory_resource*` (ADR-0010)
/// rather than using the global default: allocation is then attributable to
/// the one book that owns it, measurable per-instance by a counting resource
/// in tests, and has no hidden global mutable state (which
/// `configs/architecture_rules.yaml`'s `mutable_globals_forbidden_in` bans in
/// this layer regardless). `reserve()` up front is what lets a steady-state
/// add/cancel/match cycle perform zero calls to that resource once warmed —
/// `tests/cpp/property/test_allocation_counters.cpp` measures it.
///
/// Deliberately not the M8 pool: no generation counters, no cross-book
/// sharing, no zero-allocation latency claim (AEGIS-042/043 own those).
class OrderStorage {
 public:
  explicit OrderStorage(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
      : nodes_(resource), free_list_(resource) {}

  /// Reserves both the node slab and the free list to `capacity`: a run that
  /// never holds more than `capacity` live orders simultaneously, and never
  /// frees more than `capacity` of them without an intervening allocate,
  /// cannot grow either container past this call.
  void reserve(std::size_t capacity) {
    nodes_.reserve(capacity);
    free_list_.reserve(capacity);
  }

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
  std::pmr::vector<OrderNode> nodes_;
  std::pmr::vector<std::size_t> free_list_;
};

}  // namespace aegis::exchange
