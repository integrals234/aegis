#include "cpp/participant/oms/order_manager.hpp"

#include <utility>

namespace aegis::participant::oms {

using events::exchange::CancelOrderCommand;
using events::exchange::ModifyOrderCommand;
using events::exchange::NewOrderCommand;
using events::exchange::OrderAcceptedEvent;
using events::exchange::OrderRejectedEvent;
using events::exchange::OrderReplacedEvent;
using events::exchange::OrderTerminatedEvent;
using events::exchange::TerminationReason;
using events::exchange::TradeEvent;

std::uint64_t OrderManager::submit_new_order(
    std::uint32_t instrument_id, std::uint64_t participant_id, events::exchange::Side side,
    events::exchange::OrderType order_type, std::int64_t price_units, std::int64_t quantity_units) {
  const std::uint64_t client_order_id = next_client_order_id_++;
  TrackedOrder tracked;
  tracked.client_order_id = client_order_id;
  tracked.instrument_id = instrument_id;
  tracked.participant_id = participant_id;
  tracked.side = side;
  static_cast<void>(tracked.lifecycle.transition(OrderState::kRiskPending));

  NewOrderCommand command;
  command.instrument_id = instrument_id;
  command.participant_id = participant_id;
  command.client_order_id = client_order_id;
  command.side = side;
  command.order_type = order_type;
  command.price_units = price_units;
  command.quantity_units = quantity_units;

  const RiskDecision decision = risk_gate_->decide(command);
  if (decision.verdict == RiskVerdict::kReject) {
    static_cast<void>(tracked.lifecycle.transition(OrderState::kRejected));
  } else {
    if (decision.verdict == RiskVerdict::kResize) {
      command.quantity_units = decision.resized_quantity_units;
    }
    tracked.original_quantity_units = command.quantity_units;
    tracked.remaining_units = command.quantity_units;
    static_cast<void>(tracked.lifecycle.transition(OrderState::kSubmitted));
    // A send failure leaves the order Submitted and unacknowledged; there is
    // no separate "send failed" state in the frozen list (order_state.hpp).
    static_cast<void>(adapter_->submit(command));
  }

  orders_by_client_id_.emplace(client_order_id, std::move(tracked));
  return client_order_id;
}

bool OrderManager::cancel_order(std::uint64_t client_order_id) {
  const auto it = orders_by_client_id_.find(client_order_id);
  if (it == orders_by_client_id_.end()) {
    return false;
  }
  TrackedOrder& tracked = it->second;
  if (tracked.exchange_order_id == 0 ||
      !is_legal_transition(tracked.lifecycle.state(), OrderState::kCancelPending)) {
    return false;
  }

  CancelOrderCommand command;
  command.instrument_id = tracked.instrument_id;
  command.participant_id = tracked.participant_id;
  command.order_id = tracked.exchange_order_id;
  if (!adapter_->cancel(command)) {
    return false;
  }
  return tracked.lifecycle.transition(OrderState::kCancelPending);
}

bool OrderManager::modify_order(std::uint64_t client_order_id, std::int64_t new_price_units,
                                std::int64_t new_quantity_units) {
  const auto it = orders_by_client_id_.find(client_order_id);
  if (it == orders_by_client_id_.end()) {
    return false;
  }
  TrackedOrder& tracked = it->second;
  if (tracked.exchange_order_id == 0 || tracked.lifecycle.is_terminal()) {
    return false;
  }

  ModifyOrderCommand command;
  command.instrument_id = tracked.instrument_id;
  command.participant_id = tracked.participant_id;
  command.order_id = tracked.exchange_order_id;
  command.new_price_units = new_price_units;
  command.new_quantity_units = new_quantity_units;
  return adapter_->modify(command);
}

void OrderManager::handle_order_accepted(const OrderAcceptedEvent& event) {
  // client_order_id is scoped to (participant_id, client_order_id), not
  // globally unique (ADR-0011) -- another participant's order can carry the
  // same client_order_id this manager already assigned, so participant_id
  // must agree too before this event is accepted as ours.
  const auto it = orders_by_client_id_.find(event.client_order_id);
  if (it == orders_by_client_id_.end() || it->second.participant_id != event.participant_id) {
    return;
  }
  TrackedOrder& tracked = it->second;
  tracked.exchange_order_id = event.order_id;
  exchange_to_client_id_[event.order_id] = event.client_order_id;
  static_cast<void>(tracked.lifecycle.transition(OrderState::kAcknowledged));
}

void OrderManager::handle_order_rejected(const OrderRejectedEvent& event) {
  if (event.client_order_id != 0) {
    // A new-order rejection: the order never existed at the exchange. Same
    // participant-scoping caveat as handle_order_accepted.
    const auto it = orders_by_client_id_.find(event.client_order_id);
    if (it == orders_by_client_id_.end() || it->second.participant_id != event.participant_id) {
      return;
    }
    static_cast<void>(it->second.lifecycle.transition(OrderState::kRejected));
    return;
  }
  if (event.order_id == 0) {
    return;  // Malformed: ADR-0011 guarantees exactly one of the two is nonzero.
  }
  TrackedOrder* tracked = find_mutable_by_exchange_order_id(event.order_id);
  if (tracked == nullptr) {
    return;
  }
  if (tracked->lifecycle.state() == OrderState::kCancelPending) {
    // A rejected cancel: the order remains live. The frozen state list has
    // no separate "cancel rejected" state, so it returns to whichever state
    // it was actually in before the cancel was attempted -- inferred from
    // whether any fill has landed, since that is not itself forgotten.
    const OrderState revert_to = tracked->cumulative_filled_units > 0 ? OrderState::kPartiallyFilled
                                                                      : OrderState::kAcknowledged;
    static_cast<void>(tracked->lifecycle.transition(revert_to));
  }
  // Otherwise this is a rejected modify: no lifecycle state change, since
  // modify never entered a pending state to begin with (order_manager.hpp).
}

void OrderManager::handle_order_replaced(const OrderReplacedEvent& event) {
  TrackedOrder* old_tracked = find_mutable_by_exchange_order_id(event.old_order_id);
  if (old_tracked == nullptr) {
    return;
  }
  exchange_to_client_id_.erase(event.old_order_id);
  // The same participant-tracked order continues under a new exchange
  // identity (ADR-0011's cancel-replace). Simplifying choice, stated
  // plainly: `remaining_units` becomes the replacement's own quantity
  // outright, not netted against fills the terminated old order already
  // had -- the exchange treats the replacement as a genuinely new resting
  // order, and this class does not claim finer-grained accounting than
  // that.
  old_tracked->original_quantity_units = event.quantity_units;
  old_tracked->remaining_units = event.quantity_units;
  old_tracked->exchange_order_id = event.new_order_id;
  exchange_to_client_id_[event.new_order_id] = old_tracked->client_order_id;
}

void OrderManager::handle_trade(const TradeEvent& event) {
  apply_fill_to(event.maker_order_id, event.quantity_units);
  apply_fill_to(event.taker_order_id, event.quantity_units);
}

void OrderManager::apply_fill_to(std::uint64_t exchange_order_id,
                                 std::int64_t fill_quantity_units) {
  TrackedOrder* tracked = find_mutable_by_exchange_order_id(exchange_order_id);
  if (tracked == nullptr) {
    return;
  }
  tracked->cumulative_filled_units += fill_quantity_units;
  tracked->remaining_units -= fill_quantity_units;
  // Always kPartiallyFilled here, even if remaining just reached zero: the
  // authoritative "fully filled" transition waits for the exchange's own
  // OrderTerminatedEvent{kFilled} (handle_order_terminated), never inferred
  // from arithmetic alone (ADR-0011: exactly one termination event per
  // order, with exactly one reason).
  static_cast<void>(tracked->lifecycle.transition(OrderState::kPartiallyFilled));
}

void OrderManager::handle_order_terminated(const OrderTerminatedEvent& event) {
  TrackedOrder* tracked = find_mutable_by_exchange_order_id(event.order_id);
  if (tracked == nullptr) {
    return;
  }
  const OrderState target =
      event.reason == TerminationReason::kFilled ? OrderState::kFilled : OrderState::kCancelled;
  static_cast<void>(tracked->lifecycle.transition(target));
}

const TrackedOrder* OrderManager::find_by_client_order_id(std::uint64_t client_order_id) const {
  const auto it = orders_by_client_id_.find(client_order_id);
  return it == orders_by_client_id_.end() ? nullptr : &it->second;
}

const TrackedOrder* OrderManager::find_by_exchange_order_id(std::uint64_t exchange_order_id) const {
  const auto exch_it = exchange_to_client_id_.find(exchange_order_id);
  if (exch_it == exchange_to_client_id_.end()) {
    return nullptr;
  }
  return find_by_client_order_id(exch_it->second);
}

TrackedOrder* OrderManager::find_mutable_by_exchange_order_id(std::uint64_t exchange_order_id) {
  const auto exch_it = exchange_to_client_id_.find(exchange_order_id);
  if (exch_it == exchange_to_client_id_.end()) {
    return nullptr;
  }
  const auto order_it = orders_by_client_id_.find(exch_it->second);
  return order_it == orders_by_client_id_.end() ? nullptr : &order_it->second;
}

}  // namespace aegis::participant::oms
