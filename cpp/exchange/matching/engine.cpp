#include "cpp/exchange/matching/engine.hpp"

#include <optional>

namespace aegis::exchange {
namespace {

using events::exchange::NewOrderCommand;
using events::exchange::OrderAcceptedEvent;
using events::exchange::OrderRejectedEvent;
using events::exchange::OrderTerminatedEvent;
using events::exchange::TerminationReason;
using events::exchange::TradeEvent;

EmittedEvent make_event(events::MessageType type, std::vector<std::byte> payload) {
  return EmittedEvent{.message_type = type, .payload = std::move(payload)};
}

}  // namespace

std::vector<EmittedEvent> MatchingEngine::apply_new_order(
    OrderBook& book, const InstrumentSpec& spec, const NewOrderCommand& command,
    events::CommandSequence command_sequence) {
  std::vector<EmittedEvent> events_out;

  const QuantityUnits quantity{command.quantity_units};
  const auto quantity_reject = validate_quantity(spec, quantity);
  const std::optional<RejectReason> price_reject =
      command.order_type == OrderType::kLimit
          ? validate_price(spec, PriceUnits{command.price_units})
          : std::nullopt;

  const auto reject_reason = quantity_reject.has_value() ? quantity_reject : price_reject;
  if (reject_reason.has_value()) {
    events_out.push_back(
        make_event(events::MessageType::kOrderRejected,
                   encode(OrderRejectedEvent{.causing_command_sequence = command_sequence.value(),
                                             .instrument_id = command.instrument_id,
                                             .participant_id = command.participant_id,
                                             .client_order_id = command.client_order_id,
                                             .reason = *reject_reason})));
    return events_out;
  }

  const OrderId order_id{next_order_id_};
  ++next_order_id_;
  const Priority priority = Priority::from(command_sequence);

  events_out.push_back(
      make_event(events::MessageType::kOrderAccepted,
                 encode(OrderAcceptedEvent{.causing_command_sequence = command_sequence.value(),
                                           .order_id = order_id.value(),
                                           .instrument_id = command.instrument_id,
                                           .participant_id = command.participant_id,
                                           .client_order_id = command.client_order_id,
                                           .side = command.side,
                                           .order_type = command.order_type,
                                           .price_units = command.price_units,
                                           .quantity_units = quantity.value()})));

  const std::optional<PriceUnits> limit_price = command.order_type == OrderType::kLimit
                                                    ? std::optional{PriceUnits{command.price_units}}
                                                    : std::nullopt;
  const std::vector<Pairing> pairings = policy_->match(book, command.side, limit_price, quantity);

  QuantityUnits remaining = quantity;
  for (const auto& pairing : pairings) {
    const OrderNode* maker_before = book.find(pairing.maker_order_id);
    const auto maker_participant_id = maker_before->participant_id;
    const auto maker_price = maker_before->price_units;

    const QuantityUnits maker_remaining =
        book.record_fill(pairing.maker_order_id, pairing.fill_quantity);
    remaining -= pairing.fill_quantity;

    events_out.push_back(
        make_event(events::MessageType::kTrade,
                   encode(TradeEvent{.causing_command_sequence = command_sequence.value(),
                                     .instrument_id = command.instrument_id,
                                     .price_units = maker_price.value(),
                                     .quantity_units = pairing.fill_quantity.value(),
                                     .maker_order_id = pairing.maker_order_id.value(),
                                     .taker_order_id = order_id.value(),
                                     .maker_participant_id = maker_participant_id.value(),
                                     .taker_participant_id = command.participant_id,
                                     .taker_side = command.side})));

    if (maker_remaining.value() == 0) {
      book.cancel(pairing.maker_order_id);
      events_out.push_back(make_event(
          events::MessageType::kOrderTerminated,
          encode(OrderTerminatedEvent{.causing_command_sequence = command_sequence.value(),
                                      .order_id = pairing.maker_order_id.value(),
                                      .reason = TerminationReason::kFilled,
                                      .cancelled_quantity_delta_units = 0})));
    }
  }

  if (remaining.value() == 0) {
    events_out.push_back(
        make_event(events::MessageType::kOrderTerminated,
                   encode(OrderTerminatedEvent{.causing_command_sequence = command_sequence.value(),
                                               .order_id = order_id.value(),
                                               .reason = TerminationReason::kFilled,
                                               .cancelled_quantity_delta_units = 0})));
  } else if (command.order_type == OrderType::kMarket) {
    events_out.push_back(make_event(
        events::MessageType::kOrderTerminated,
        encode(OrderTerminatedEvent{.causing_command_sequence = command_sequence.value(),
                                    .order_id = order_id.value(),
                                    .reason = TerminationReason::kResidualCanceled,
                                    .cancelled_quantity_delta_units = remaining.value()})));
  } else {
    book.add(OrderNode{.order_id = order_id,
                       .instrument_id = InstrumentId{command.instrument_id},
                       .participant_id = ParticipantId{command.participant_id},
                       .client_order_id = ClientOrderId{command.client_order_id},
                       .side = command.side,
                       .order_type = command.order_type,
                       .price_units = PriceUnits{command.price_units},
                       .original_quantity = quantity,
                       .cumulative_filled = quantity - remaining,
                       .cancelled_quantity = kZeroQuantity,
                       .remaining = remaining,
                       .priority = priority});
  }

  return events_out;
}

}  // namespace aegis::exchange
