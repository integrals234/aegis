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
    case OrderState::kAcknowledged:
      return to == OrderState::kPartiallyFilled || to == OrderState::kFilled ||
             to == OrderState::kCancelPending || to == OrderState::kExpired;
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
