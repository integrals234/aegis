#include "cpp/exchange/order_book/invariants.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace aegis::exchange {
namespace {

std::string describe_price(PriceUnits price) { return std::to_string(price.value()); }
std::string describe_order(OrderId order_id) { return std::to_string(order_id.value()); }

/// The slab-wide checks: order-index/queue bijection (half of it — the
/// other half is closed by `walked_live_orders` matching `live_count()` in
/// the caller), the free list disjoint from the live set (implied: a slot
/// is either free or checked here as live, never both, by construction of
/// the `is_free` scan), and `0 < remaining <= original_quantity`.
void check_slab(const OrderBook& book, std::vector<std::string>& errors) {
  const auto& storage = book.storage();
  for (std::size_t index = 0; index < storage.size(); ++index) {
    if (storage.is_free(index)) {
      continue;
    }
    const auto& node = storage.node(index);
    if (node.remaining.value() <= 0 || node.remaining > node.original_quantity) {
      errors.push_back("order " + describe_order(node.order_id) +
                       ": remaining must satisfy 0 < remaining <= original_quantity, got " +
                       std::to_string(node.remaining.value()) + " of " +
                       std::to_string(node.original_quantity.value()));
    }
    const auto* found = book.find(node.order_id);
    if (found != &node) {
      errors.push_back("order " + describe_order(node.order_id) +
                       ": not reachable via OrderBook::find at its own slab slot "
                       "(order-index/slab bijection broken)");
    }
  }
}

/// Walks one side's levels from best to worst, checking level ordering
/// (structural), each level's queue (aggregate, count, strictly increasing
/// priority — structural), and, for `kQuiescent` only, that no emptied level
/// is retained. Returns the number of live orders walked, so the caller can
/// close the other half of the order-index/queue bijection: every live slab
/// entry must be reachable from some level's queue.
std::size_t check_side(const OrderBook& book, Side side, InvariantScope scope,
                       std::vector<std::string>& errors) {
  const auto& level_index = book.levels(side);
  std::size_t total_walked = 0;
  std::optional<PriceUnits> previous_price;
  std::optional<PriceUnits> price = level_index.best_price();

  while (price.has_value()) {
    const auto* level = book.level_at(side, *price);
    if (level == nullptr) {
      errors.push_back("level at price " + describe_price(*price) + " vanished during traversal");
      break;
    }

    if (previous_price.has_value()) {
      const bool ordered =
          side == Side::kBuy ? (*price < *previous_price) : (*price > *previous_price);
      if (!ordered) {
        errors.push_back("levels are not correctly ordered around price " + describe_price(*price));
      }
    }

    if (scope == InvariantScope::kQuiescent && level->empty()) {
      errors.push_back("empty level retained in the index at price " + describe_price(*price));
    }

    QuantityUnits summed_remaining{0};
    std::size_t walked_here = 0;
    std::optional<Priority> previous_priority;
    std::size_t node_index = level->head_index;
    while (node_index != kInvalidIndex) {
      const auto& node = book.storage().node(node_index);
      summed_remaining += node.remaining;
      ++walked_here;
      if (previous_priority.has_value() && !(*previous_priority < node.priority)) {
        errors.push_back("priority not strictly increasing along the queue at price " +
                         describe_price(*price));
      }
      previous_priority = node.priority;
      node_index = node.next_index;
    }

    if (summed_remaining != level->aggregate_quantity) {
      errors.push_back("aggregate_quantity mismatch at price " + describe_price(*price) +
                       ": level says " + std::to_string(level->aggregate_quantity.value()) +
                       ", queue sums to " + std::to_string(summed_remaining.value()));
    }
    if (walked_here != level->order_count) {
      errors.push_back("order_count mismatch at price " + describe_price(*price));
    }

    total_walked += walked_here;
    previous_price = price;
    price = level_index.next_price_after(*price);
  }
  return total_walked;
}

void check_quiescent_only(const OrderBook& book, std::vector<std::string>& errors) {
  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  if (bid.has_value() && ask.has_value() && !(*bid < *ask)) {
    errors.push_back("book is crossed: best_bid (" + describe_price(*bid) + ") >= best_ask (" +
                     describe_price(*ask) + ")");
  }

  const auto& storage = book.storage();
  for (std::size_t index = 0; index < storage.size(); ++index) {
    if (storage.is_free(index)) {
      continue;
    }
    const auto& node = storage.node(index);
    const auto sum = node.cumulative_filled + node.remaining + node.cancelled_quantity;
    if (sum != node.original_quantity) {
      errors.push_back("P1 conservation violated for order " + describe_order(node.order_id) +
                       ": " + std::to_string(node.cumulative_filled.value()) + " + " +
                       std::to_string(node.remaining.value()) + " + " +
                       std::to_string(node.cancelled_quantity.value()) + " != original_quantity " +
                       std::to_string(node.original_quantity.value()));
    }
  }
}

}  // namespace

std::vector<std::string> check_invariants(const OrderBook& book, InvariantScope scope) {
  std::vector<std::string> errors;
  check_slab(book, errors);
  const auto walked_bids = check_side(book, Side::kBuy, scope, errors);
  const auto walked_asks = check_side(book, Side::kSell, scope, errors);
  if (walked_bids + walked_asks != book.live_order_count()) {
    errors.push_back(
        "order-index/queue bijection broken: " + std::to_string(walked_bids + walked_asks) +
        " orders reachable from level queues, but " + std::to_string(book.live_order_count()) +
        " live in the book");
  }
  if (scope == InvariantScope::kQuiescent) {
    check_quiescent_only(book, errors);
  }
  return errors;
}

}  // namespace aegis::exchange
