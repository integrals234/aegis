#include "cpp/exchange/app/exchange_node.hpp"

namespace aegis::exchange {
namespace {

using events::exchange::CancelOrderCommand;
using events::exchange::ModifyOrderCommand;
using events::exchange::NewOrderCommand;
using events::exchange::OrderRejectedEvent;

/// `kUnknownInstrument` never reaches `MatchingEngine`: there is no book to
/// hand it. `client_order_id`/`order_id` follow the same "exactly one
/// nonzero" convention as every other `OrderRejectedEvent` (ADR-0011) — a
/// `NewOrder` names a `client_order_id` and no `order_id` yet exists;
/// `Cancel`/`Modify` name an `order_id` and carry no `client_order_id`.
std::vector<EmittedEvent> unknown_instrument_reject(std::uint32_t instrument_id,
                                                    std::uint64_t participant_id,
                                                    std::uint64_t client_order_id,
                                                    std::uint64_t order_id,
                                                    events::CommandSequence command_sequence) {
  std::vector<EmittedEvent> events_out;
  events_out.push_back(EmittedEvent{
      .message_type = events::MessageType::kOrderRejected,
      .payload = encode(OrderRejectedEvent{.causing_command_sequence = command_sequence.value(),
                                           .instrument_id = instrument_id,
                                           .participant_id = participant_id,
                                           .client_order_id = client_order_id,
                                           .order_id = order_id,
                                           .reason = RejectReason::kUnknownInstrument})});
  return events_out;
}

}  // namespace

void ExchangeNode::add_instrument(const InstrumentSpec& spec, std::size_t order_capacity,
                                  std::pmr::memory_resource* resource) {
  instruments_.emplace(spec.instrument_id.value(), spec);
  auto [it, inserted] =
      books_.try_emplace(spec.instrument_id.value(), spec.instrument_id, resource);
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

std::vector<EmittedEvent> ExchangeNode::apply_new_order(const NewOrderCommand& command,
                                                        events::CommandSequence command_sequence) {
  const InstrumentId instrument_id{command.instrument_id};
  auto* target_book = book(instrument_id);
  const auto* spec = instrument(instrument_id);
  if (target_book == nullptr || spec == nullptr) {
    return unknown_instrument_reject(command.instrument_id, command.participant_id,
                                     command.client_order_id, 0, command_sequence);
  }
  return matching_engine_.apply_new_order(*target_book, *spec, command, command_sequence);
}

std::vector<EmittedEvent> ExchangeNode::apply_cancel_order(
    const CancelOrderCommand& command, events::CommandSequence command_sequence) {
  const InstrumentId instrument_id{command.instrument_id};
  auto* target_book = book(instrument_id);
  if (target_book == nullptr) {
    return unknown_instrument_reject(command.instrument_id, command.participant_id, 0,
                                     command.order_id, command_sequence);
  }
  return MatchingEngine::apply_cancel_order(*target_book, command, command_sequence);
}

std::vector<EmittedEvent> ExchangeNode::apply_modify_order(
    const ModifyOrderCommand& command, events::CommandSequence command_sequence) {
  const InstrumentId instrument_id{command.instrument_id};
  auto* target_book = book(instrument_id);
  const auto* spec = instrument(instrument_id);
  if (target_book == nullptr || spec == nullptr) {
    return unknown_instrument_reject(command.instrument_id, command.participant_id, 0,
                                     command.order_id, command_sequence);
  }
  return matching_engine_.apply_modify_order(*target_book, *spec, command, command_sequence);
}

}  // namespace aegis::exchange
