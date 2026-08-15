#include "cpp/participant/oms/order_state.hpp"

namespace aegis::participant::oms {

bool is_terminal(OrderState state) {
  switch (state) {
    case OrderState::kRejected:
    case OrderState::kFilled:
    case OrderState::kCancelled:
    case OrderState::kExpired:
      return true;
    case OrderState::kCreated:
    case OrderState::kRiskPending:
    case OrderState::kSubmitted:
    case OrderState::kAcknowledged:
    case OrderState::kPartiallyFilled:
    case OrderState::kCancelPending:
      return false;
  }
  return false;
}

bool is_legal_transition(OrderState from, OrderState to) {
  switch (from) {
    case OrderState::kCreated:
      return to == OrderState::kRiskPending;
    case OrderState::kRiskPending:
      return to == OrderState::kSubmitted || to == OrderState::kRejected;
    case OrderState::kSubmitted:
      return to == OrderState::kAcknowledged || to == OrderState::kRejected;
    // Deliberately one branch: an acknowledged order and a partially filled
    // one have exactly the same legal successors. A partial fill does not
    // restrict what may happen next -- it can fill further, complete, be
    // cancelled or expire, precisely as a resting acknowledged order can.
    // (`kPartiallyFilled -> kPartiallyFilled` is the self-loop a second
    // partial fill takes; from `kAcknowledged` the same edge is the first
    // partial fill.) Writing them as two identical branches said the same
    // thing twice and invited them to drift apart.
    case OrderState::kAcknowledged:
    case OrderState::kPartiallyFilled:
      return to == OrderState::kPartiallyFilled || to == OrderState::kFilled ||
             to == OrderState::kCancelPending || to == OrderState::kExpired;
    case OrderState::kCancelPending:
      return to == OrderState::kCancelled || to == OrderState::kFilled ||
             to == OrderState::kPartiallyFilled || to == OrderState::kAcknowledged;
    case OrderState::kRejected:
    case OrderState::kFilled:
    case OrderState::kCancelled:
    case OrderState::kExpired:
      return false;
  }
  return false;
}

}  // namespace aegis::participant::oms
