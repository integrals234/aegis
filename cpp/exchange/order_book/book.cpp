#include "cpp/exchange/order_book/book.hpp"

namespace aegis::exchange {

void OrderBook::reserve(std::size_t order_capacity) {
  storage_.reserve(order_capacity);
  order_index_.reserve(order_capacity);
}

void OrderBook::add(const OrderNode& descriptor) {
  const auto index = storage_.allocate();
  OrderNode& node = storage_.node(index);
  node = descriptor;
  node.prev_index = kInvalidIndex;
  node.next_index = kInvalidIndex;

  PriceLevel& level = levels_for(node.side).level_at(node.price_units);
  if (level.tail_index == kInvalidIndex) {
    level.head_index = index;
    level.tail_index = index;
  } else {
    storage_.node(level.tail_index).next_index = index;
    node.prev_index = level.tail_index;
    level.tail_index = index;
  }
  level.order_count += 1;
  level.aggregate_quantity += node.remaining;

  order_index_[node.order_id.value()] = index;
  live_client_ids_[{node.participant_id.value(), node.client_order_id.value()}] = node.order_id;
}

std::optional<OrderNode> OrderBook::cancel(OrderId order_id) {
  const auto found = order_index_.find(order_id.value());
  if (found == order_index_.end()) {
    return std::nullopt;
  }
  const auto index = found->second;
  const OrderNode removed = storage_.node(index);

  LevelIndex& level_index = levels_for(removed.side);
  PriceLevel* level = level_index.find(removed.price_units);

  if (removed.prev_index != kInvalidIndex) {
    storage_.node(removed.prev_index).next_index = removed.next_index;
  } else {
    level->head_index = removed.next_index;
  }
  if (removed.next_index != kInvalidIndex) {
    storage_.node(removed.next_index).prev_index = removed.prev_index;
  } else {
    level->tail_index = removed.prev_index;
  }
  level->order_count -= 1;
  level->aggregate_quantity -= removed.remaining;
  if (level->empty()) {
    level_index.erase(removed.price_units);
  }

  order_index_.erase(found);
  live_client_ids_.erase({removed.participant_id.value(), removed.client_order_id.value()});
  storage_.release(index);
  return removed;
}

const OrderNode* OrderBook::find(OrderId order_id) const {
  const auto found = order_index_.find(order_id.value());
  return found == order_index_.end() ? nullptr : &storage_.node(found->second);
}

OrderNode* OrderBook::find(OrderId order_id) {
  const auto found = order_index_.find(order_id.value());
  return found == order_index_.end() ? nullptr : &storage_.node(found->second);
}

std::optional<OrderId> OrderBook::find_live_by_client_id(ParticipantId participant_id,
                                                         ClientOrderId client_order_id) const {
  const auto found = live_client_ids_.find({participant_id.value(), client_order_id.value()});
  if (found == live_client_ids_.end()) {
    return std::nullopt;
  }
  return found->second;
}

const PriceLevel* OrderBook::level_at(Side side, PriceUnits price) const {
  return levels_for(side).find(price);
}

std::vector<OrderId> OrderBook::orders_at(Side side, PriceUnits price) const {
  std::vector<OrderId> result;
  const PriceLevel* level = level_at(side, price);
  if (level == nullptr) {
    return result;
  }
  result.reserve(level->order_count);
  std::size_t index = level->head_index;
  while (index != kInvalidIndex) {
    const OrderNode& node = storage_.node(index);
    result.push_back(node.order_id);
    index = node.next_index;
  }
  return result;
}

}  // namespace aegis::exchange
