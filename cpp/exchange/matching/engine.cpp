#include "cpp/exchange/matching/engine.hpp"

#include <optional>

namespace aegis::exchange {
namespace {

using events::exchange::CancelOrderCommand;
using events::exchange::ModifyOrderCommand;
using events::exchange::NewOrderCommand;
using events::exchange::OrderAcceptedEvent;
using events::exchange::OrderModifiedEvent;
using events::exchange::OrderRejectedEvent;
using events::exchange::OrderReplacedEvent;
using events::exchange::OrderTerminatedEvent;
using events::exchange::TerminationReason;
using events::exchange::TradeEvent;

EmittedEvent make_event(events::MessageType type, std::vector<std::byte> payload) {
  return EmittedEvent{.message_type = type, .payload = std::move(payload)};
}

/// The lookup shared by cancel and modify (ADR-0011): both target an
/// already-live `OrderId`, not a fresh submission. `node` is set on success;
/// otherwise `reject` names why. `node` is a raw pointer into the book's
/// slab and is invalidated the moment the caller inserts a new order
/// (`OrderBook::add`) — callers that need fields from it across such a call
/// must copy them out first.
struct OwnedOrderLookup {
  OrderNode* node{nullptr};
  std::optional<RejectReason> reject;
};

OwnedOrderLookup find_live_owned_order(OrderBook& book, OrderId order_id,
                                       ParticipantId participant_id) {
  OrderNode* node = book.find(order_id);
  if (node == nullptr) {
    return {.node = nullptr, .reject = RejectReason::kUnknownOrderId};
  }
  if (node->participant_id != participant_id) {
    return {.node = nullptr, .reject = RejectReason::kNotOrderOwner};
  }
  return {.node = node, .reject = std::nullopt};
}

/// Walks the policy's pairings against `book`, applying each fill and
/// emitting `Trade` plus, for any maker fully consumed, `OrderTerminated`.
/// Shared by `apply_new_order` and `apply_modify_order`'s cancel-replace
/// path, both of which match a taker exactly the same way afterward. Returns
/// the taker's unfilled remainder.
QuantityUnits match_and_apply_fills(OrderBook& book, const MatchingPolicy& policy, Side side,
                                    std::optional<PriceUnits> limit_price, QuantityUnits quantity,
                                    OrderId taker_order_id, std::uint64_t taker_participant_id,
                                    std::uint32_t instrument_id,
                                    std::uint64_t causing_command_sequence,
                                    std::vector<EmittedEvent>& events_out) {
  const std::vector<Pairing> pairings = policy.match(book, side, limit_price, quantity);
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
                   encode(TradeEvent{.causing_command_sequence = causing_command_sequence,
                                     .instrument_id = instrument_id,
                                     .price_units = maker_price.value(),
                                     .quantity_units = pairing.fill_quantity.value(),
                                     .maker_order_id = pairing.maker_order_id.value(),
                                     .taker_order_id = taker_order_id.value(),
                                     .maker_participant_id = maker_participant_id.value(),
                                     .taker_participant_id = taker_participant_id,
                                     .taker_side = side})));

    if (maker_remaining.value() == 0) {
      book.cancel(pairing.maker_order_id);
      events_out.push_back(make_event(
          events::MessageType::kOrderTerminated,
          encode(OrderTerminatedEvent{.causing_command_sequence = causing_command_sequence,
                                      .order_id = pairing.maker_order_id.value(),
                                      .reason = TerminationReason::kFilled,
                                      .cancelled_quantity_delta_units = 0})));
    }
  }
  return remaining;
}

}  // namespace

std::vector<EmittedEvent> MatchingEngine::apply_new_order(
    OrderBook& book, const InstrumentSpec& spec, const NewOrderCommand& command,
    events::CommandSequence command_sequence) {
  std::vector<EmittedEvent> events_out;

  const QuantityUnits quantity{command.quantity_units};
  const auto quantity_reject = validate_quantity(spec, quantity);
  // A market order carries no price (ADR-0011): a nonzero price is a
  // validation failure, decided before the order exists, exactly like an
  // off-tick limit price — never a book-state outcome.
  std::optional<RejectReason> price_reject;
  if (command.order_type == OrderType::kLimit) {
    price_reject = validate_price(spec, PriceUnits{command.price_units});
  } else if (command.price_units != 0) {
    price_reject = RejectReason::kPriceOnMarketOrder;
  }

  const auto reject_reason = quantity_reject.has_value() ? quantity_reject : price_reject;
  if (reject_reason.has_value()) {
    events_out.push_back(
        make_event(events::MessageType::kOrderRejected,
                   encode(OrderRejectedEvent{.causing_command_sequence = command_sequence.value(),
                                             .instrument_id = command.instrument_id,
                                             .participant_id = command.participant_id,
                                             .client_order_id = command.client_order_id,
                                             .order_id = 0,
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
  const QuantityUnits remaining = match_and_apply_fills(
      book, *policy_, command.side, limit_price, quantity, order_id, command.participant_id,
      command.instrument_id, command_sequence.value(), events_out);

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

std::vector<EmittedEvent> MatchingEngine::apply_cancel_order(
    OrderBook& book, const CancelOrderCommand& command, events::CommandSequence command_sequence) {
  std::vector<EmittedEvent> events_out;

  const auto lookup =
      find_live_owned_order(book, OrderId{command.order_id}, ParticipantId{command.participant_id});
  if (lookup.reject.has_value()) {
    events_out.push_back(
        make_event(events::MessageType::kOrderRejected,
                   encode(OrderRejectedEvent{.causing_command_sequence = command_sequence.value(),
                                             .instrument_id = command.instrument_id,
                                             .participant_id = command.participant_id,
                                             .client_order_id = 0,
                                             .order_id = command.order_id,
                                             .reason = *lookup.reject})));
    return events_out;
  }

  const QuantityUnits remaining_before_cancel = lookup.node->remaining;
  book.cancel(OrderId{command.order_id});
  events_out.push_back(
      make_event(events::MessageType::kOrderTerminated,
                 encode(OrderTerminatedEvent{
                     .causing_command_sequence = command_sequence.value(),
                     .order_id = command.order_id,
                     .reason = TerminationReason::kCanceled,
                     .cancelled_quantity_delta_units = remaining_before_cancel.value()})));
  return events_out;
}

std::vector<EmittedEvent> MatchingEngine::apply_modify_order(
    OrderBook& book, const InstrumentSpec& spec, const ModifyOrderCommand& command,
    events::CommandSequence command_sequence) {
  std::vector<EmittedEvent> events_out;

  const auto lookup =
      find_live_owned_order(book, OrderId{command.order_id}, ParticipantId{command.participant_id});
  if (lookup.reject.has_value()) {
    events_out.push_back(
        make_event(events::MessageType::kOrderRejected,
                   encode(OrderRejectedEvent{.causing_command_sequence = command_sequence.value(),
                                             .instrument_id = command.instrument_id,
                                             .participant_id = command.participant_id,
                                             .client_order_id = 0,
                                             .order_id = command.order_id,
                                             .reason = *lookup.reject})));
    return events_out;
  }

  const OrderNode* existing = lookup.node;
  const QuantityUnits new_total{command.new_quantity_units};

  // ADR-0011: new_quantity_units is the order's new *total* (FIX OrderQty
  // convention) — the only interpretation under which "below what has
  // already filled" is a checkable condition.
  std::optional<RejectReason> reject_reason;
  if (new_total < existing->cumulative_filled) {
    reject_reason = RejectReason::kModifyBelowFilled;
  } else {
    reject_reason = validate_quantity(spec, new_total);
    if (!reject_reason.has_value()) {
      reject_reason = validate_price(spec, PriceUnits{command.new_price_units});
    }
  }
  if (reject_reason.has_value()) {
    events_out.push_back(
        make_event(events::MessageType::kOrderRejected,
                   encode(OrderRejectedEvent{.causing_command_sequence = command_sequence.value(),
                                             .instrument_id = command.instrument_id,
                                             .participant_id = command.participant_id,
                                             .client_order_id = 0,
                                             .order_id = command.order_id,
                                             .reason = *reject_reason})));
    return events_out;
  }

  const QuantityUnits implied_remaining = new_total - existing->cumulative_filled;
  const PriceUnits new_price{command.new_price_units};
  const bool price_unchanged = new_price == existing->price_units;
  const bool is_decrease = price_unchanged && implied_remaining <= existing->remaining;

  if (is_decrease) {
    const QuantityUnits delta = existing->remaining - implied_remaining;
    book.apply_quantity_decrease(OrderId{command.order_id}, implied_remaining);
    events_out.push_back(
        make_event(events::MessageType::kOrderModified,
                   encode(OrderModifiedEvent{.causing_command_sequence = command_sequence.value(),
                                             .order_id = command.order_id,
                                             .new_remaining_units = implied_remaining.value(),
                                             .cancelled_quantity_delta_units = delta.value()})));
    return events_out;
  }

  // Cancel-replace: capture what the replacement inherits before the old
  // order's slab slot is released.
  const Side side = existing->side;
  const ClientOrderId client_order_id = existing->client_order_id;
  const QuantityUnits remaining_before_cancel = existing->remaining;

  book.cancel(OrderId{command.order_id});
  events_out.push_back(
      make_event(events::MessageType::kOrderTerminated,
                 encode(OrderTerminatedEvent{
                     .causing_command_sequence = command_sequence.value(),
                     .order_id = command.order_id,
                     .reason = TerminationReason::kReplaced,
                     .cancelled_quantity_delta_units = remaining_before_cancel.value()})));

  const OrderId new_order_id{next_order_id_};
  ++next_order_id_;
  const Priority new_priority = Priority::from(command_sequence);

  events_out.push_back(
      make_event(events::MessageType::kOrderReplaced,
                 encode(OrderReplacedEvent{.causing_command_sequence = command_sequence.value(),
                                           .old_order_id = command.order_id,
                                           .new_order_id = new_order_id.value(),
                                           .instrument_id = command.instrument_id,
                                           .participant_id = command.participant_id,
                                           .client_order_id = client_order_id.value(),
                                           .side = side,
                                           .order_type = OrderType::kLimit,
                                           .price_units = command.new_price_units,
                                           .quantity_units = new_total.value()})));

  const QuantityUnits remaining = match_and_apply_fills(
      book, *policy_, side, std::optional{new_price}, new_total, new_order_id,
      command.participant_id, command.instrument_id, command_sequence.value(), events_out);

  if (remaining.value() == 0) {
    events_out.push_back(
        make_event(events::MessageType::kOrderTerminated,
                   encode(OrderTerminatedEvent{.causing_command_sequence = command_sequence.value(),
                                               .order_id = new_order_id.value(),
                                               .reason = TerminationReason::kFilled,
                                               .cancelled_quantity_delta_units = 0})));
  } else {
    book.add(OrderNode{.order_id = new_order_id,
                       .instrument_id = InstrumentId{command.instrument_id},
                       .participant_id = ParticipantId{command.participant_id},
                       .client_order_id = client_order_id,
                       .side = side,
                       .order_type = OrderType::kLimit,
                       .price_units = new_price,
                       .original_quantity = new_total,
                       .cumulative_filled = new_total - remaining,
                       .cancelled_quantity = kZeroQuantity,
                       .remaining = remaining,
                       .priority = new_priority});
  }

  return events_out;
}

}  // namespace aegis::exchange
