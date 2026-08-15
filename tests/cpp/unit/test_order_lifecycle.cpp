#include <array>

#include <gtest/gtest.h>

#include "cpp/participant/oms/order_lifecycle.hpp"

/// AEGIS-108: invalid transitions are rejected; transition tests cover all
/// states.
namespace {

using aegis::participant::oms::is_legal_transition;
using aegis::participant::oms::is_terminal;
using aegis::participant::oms::OrderLifecycle;
using aegis::participant::oms::OrderState;

constexpr std::array<OrderState, 10> kAllStates{
    OrderState::kCreated,   OrderState::kRiskPending,   OrderState::kRejected,
    OrderState::kSubmitted, OrderState::kAcknowledged,  OrderState::kPartiallyFilled,
    OrderState::kFilled,    OrderState::kCancelPending, OrderState::kCancelled,
    OrderState::kExpired,
};

TEST(OrderLifecycle, TerminalStatesAcceptNoOutgoingTransition) {
  for (const OrderState terminal :
       {OrderState::kRejected, OrderState::kFilled, OrderState::kCancelled, OrderState::kExpired}) {
    ASSERT_TRUE(is_terminal(terminal));
    for (const OrderState candidate : kAllStates) {
      EXPECT_FALSE(is_legal_transition(terminal, candidate))
          << "terminal state incorrectly allowed a transition to " << static_cast<int>(candidate);
    }
  }
}

TEST(OrderLifecycle, NonTerminalStatesAreNotReportedTerminal) {
  for (const OrderState state :
       {OrderState::kCreated, OrderState::kRiskPending, OrderState::kSubmitted,
        OrderState::kAcknowledged, OrderState::kPartiallyFilled, OrderState::kCancelPending}) {
    EXPECT_FALSE(is_terminal(state));
  }
}

TEST(OrderLifecycle, EveryCreatedToSubmittedPathPassesThroughRiskPending) {
  // The only legal successor of kCreated is kRiskPending -- there is no
  // direct kCreated -> kSubmitted transition, structurally enforcing the
  // mandatory risk gate.
  EXPECT_TRUE(is_legal_transition(OrderState::kCreated, OrderState::kRiskPending));
  EXPECT_FALSE(is_legal_transition(OrderState::kCreated, OrderState::kSubmitted));
  for (const OrderState candidate : kAllStates) {
    if (candidate == OrderState::kRiskPending) {
      continue;
    }
    EXPECT_FALSE(is_legal_transition(OrderState::kCreated, candidate));
  }
}

TEST(OrderLifecycle, PartiallyFilledSelfLoopIsLegal) {
  EXPECT_TRUE(is_legal_transition(OrderState::kPartiallyFilled, OrderState::kPartiallyFilled));
}

TEST(OrderLifecycle, CancelPendingCanRaceAFillArriving) {
  // A fill can race an in-flight cancel and arrive first (AEGIS-112).
  EXPECT_TRUE(is_legal_transition(OrderState::kCancelPending, OrderState::kFilled));
  EXPECT_TRUE(is_legal_transition(OrderState::kCancelPending, OrderState::kPartiallyFilled));
  EXPECT_TRUE(is_legal_transition(OrderState::kCancelPending, OrderState::kCancelled));
}

TEST(OrderLifecycle, EveryStatePairIsExercised) {
  // The exhaustive 10x10 table this ADR-0023 fixes -- every pair is asked,
  // not merely the ones an individual test above happens to name.
  int legal_count = 0;
  for (const OrderState from : kAllStates) {
    for (const OrderState to : kAllStates) {
      if (is_legal_transition(from, to)) {
        ++legal_count;
        EXPECT_FALSE(is_terminal(from)) << "a terminal state must have no legal transition";
      }
    }
  }
  EXPECT_GT(legal_count, 0);
}

TEST(OrderLifecycle, TransitionAppliesOnSuccessAndLeavesStateUnchangedOnFailure) {
  OrderLifecycle lifecycle;
  EXPECT_EQ(lifecycle.state(), OrderState::kCreated);

  EXPECT_FALSE(lifecycle.transition(OrderState::kFilled));  // Illegal: skips risk/submit/ack.
  EXPECT_EQ(lifecycle.state(), OrderState::kCreated);

  EXPECT_TRUE(lifecycle.transition(OrderState::kRiskPending));
  EXPECT_TRUE(lifecycle.transition(OrderState::kSubmitted));
  EXPECT_TRUE(lifecycle.transition(OrderState::kAcknowledged));
  EXPECT_TRUE(lifecycle.transition(OrderState::kFilled));
  EXPECT_TRUE(lifecycle.is_terminal());

  EXPECT_FALSE(lifecycle.transition(OrderState::kCancelled));  // Terminal: nothing else is legal.
  EXPECT_EQ(lifecycle.state(), OrderState::kFilled);
}

TEST(OrderLifecycle, RejectionIsReachableFromRiskPendingAndSubmitted) {
  OrderLifecycle from_risk;
  EXPECT_TRUE(from_risk.transition(OrderState::kRiskPending));
  EXPECT_TRUE(from_risk.transition(OrderState::kRejected));

  OrderLifecycle from_submitted;
  EXPECT_TRUE(from_submitted.transition(OrderState::kRiskPending));
  EXPECT_TRUE(from_submitted.transition(OrderState::kSubmitted));
  EXPECT_TRUE(from_submitted.transition(OrderState::kRejected));
}

}  // namespace
