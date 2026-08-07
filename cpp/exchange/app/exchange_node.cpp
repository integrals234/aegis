#include "cpp/exchange/app/exchange_node.hpp"

namespace aegis::exchange {

void ExchangeNode::add_instrument(const InstrumentSpec& spec, std::size_t order_capacity) {
  instruments_.emplace(spec.instrument_id.value(), spec);
  auto [it, inserted] = books_.try_emplace(spec.instrument_id.value(), spec.instrument_id);
  if (inserted && order_capacity > 0) {
    it->second.reserve(order_capacity);
  }
}

const InstrumentSpec* ExchangeNode::instrument(InstrumentId id) const {
  const auto found = instruments_.find(id.value());
  return found == instruments_.end() ? nullptr : &found->second;
}

OrderBook* ExchangeNode::book(InstrumentId id) {
  const auto found = books_.find(id.value());
  return found == books_.end() ? nullptr : &found->second;
}

const OrderBook* ExchangeNode::book(InstrumentId id) const {
  const auto found = books_.find(id.value());
  return found == books_.end() ? nullptr : &found->second;
}

}  // namespace aegis::exchange
