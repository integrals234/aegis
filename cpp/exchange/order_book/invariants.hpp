#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cpp/exchange/order_book/book.hpp"

/// The debug-only book invariant validator (AEGIS-041, ADR-0010). Expensive
/// by design — a full slab scan plus a full walk of every price level on
/// both sides — and never called from the hot add/cancel/fill path.
namespace aegis::exchange {

enum class InvariantScope : std::uint8_t {
  /// Holds at every point the book is observable, including mid-match: the
  /// order-index/queue bijection, the free list disjoint from the live set,
  /// every level's `aggregate_quantity` equal to the sum of `remaining` over
  /// its queue, `0 < remaining <= original_quantity`, priorities strictly
  /// increasing along each queue, bids descending / asks ascending in the
  /// level index.
  kStructural,
  /// `kStructural` plus invariants meaningful only once a command has fully
  /// applied: the book is uncrossed (`best_bid < best_ask`), no emptied
  /// level is retained in the index, and P1 per-order conservation
  /// (ADR-0011 §4.10) holds for every live order. Checked only at command
  /// boundaries — never mid-match, where an aggressor is legitimately
  /// crossed with the book for the duration of matching.
  kQuiescent,
};

/// Returns one description string per violation found; an empty vector means
/// `book` satisfies every invariant in `scope`.
[[nodiscard]] std::vector<std::string> check_invariants(const OrderBook& book,
                                                        InvariantScope scope);

}  // namespace aegis::exchange
