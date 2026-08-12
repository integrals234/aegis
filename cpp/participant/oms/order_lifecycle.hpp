#pragma once

#include "cpp/participant/oms/order_state.hpp"

namespace aegis::participant::oms {

/// One order's state, mutated only through legal transitions (AEGIS-108).
class OrderLifecycle {
 public:
  explicit OrderLifecycle(OrderState initial = OrderState::kCreated) : state_(initial) {}

  [[nodiscard]] OrderState state() const { return state_; }
  [[nodiscard]] bool is_terminal() const { return oms::is_terminal(state_); }

  /// Applies `next` if `is_legal_transition(state(), next)`, leaving state
  /// unchanged and returning false otherwise -- a caller distinguishes "this
  /// did not happen" from a thrown exception, since an illegal request from
  /// outside (a race, an unexpected exchange response) is an ordinary
  /// business event, not a caller bug.
  [[nodiscard]] bool transition(OrderState next) {
    if (!is_legal_transition(state_, next)) {
      return false;
    }
    state_ = next;
    return true;
  }

 private:
  OrderState state_;
};

}  // namespace aegis::participant::oms
