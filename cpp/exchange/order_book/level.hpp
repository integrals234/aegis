#pragma once

#include <cstddef>
#include <limits>

#include "cpp/exchange/order_book/types.hpp"

namespace aegis::exchange {

/// Sentinel for "no neighbor" in the intrusive FIFO queue and for an unset
/// head/tail. Not a valid `OrderStorage` slot.
inline constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();

/// One price level: the FIFO queue of resting orders at that price, plus the
/// running aggregate the book maintains incrementally rather than summing on
/// every read (P3, `tests/cpp/property/test_quantity_conservation.cpp`).
///
/// `head_index`/`tail_index` are `OrderStorage` slab indices, not pointers —
/// storage growth cannot dangle them (AEGIS-037).
struct PriceLevel {
  PriceUnits price;
  QuantityUnits aggregate_quantity;
  std::size_t order_count{0};
  std::size_t head_index{kInvalidIndex};  ///< Oldest order: front of FIFO, matched first.
  std::size_t tail_index{kInvalidIndex};  ///< Newest order: back of FIFO, arrival tail.

  [[nodiscard]] bool empty() const { return order_count == 0; }
};

}  // namespace aegis::exchange
