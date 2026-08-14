#include <gtest/gtest.h>

#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/oms/recorded_response_adapter.hpp"

/// AEGIS-108 (wired), AEGIS-112, AEGIS-114: OrderManager ties the lifecycle
/// state machine, the mandatory risk seam and an ExecutionAdapter to
/// per-order tracked state. RecordedResponseAdapter here is only the
/// injected ExecutionAdapter dependency -- these tests are about
/// OrderManager's own bookkeeping, not about proving real matching
/// behaviour (that is test_participant_exchange_integration.cpp).
namespace {

using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderAcceptedEvent;
using aegis::events::exchange::OrderRejectedEvent;
using aegis::events::exchange::OrderTerminatedEvent;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::RejectReason;
using aegis::events::exchange::Side;
using aegis::events::exchange::TerminationReason;
using aegis::events::exchange::TradeEvent;
using aegis::participant::oms::OrderManager;
using aegis::participant::oms::OrderState;
using aegis::participant::oms::RecordedResponseAdapter;
using aegis::participant::oms::RiskDecision;
using aegis::participant::oms::RiskGate;
using aegis::participant::oms::RiskVerdict;

/// Test double, explicitly named as such (ADR-0023): production code ships
/// no RiskGate implementation before M5.
class AlwaysApproveRiskGate final : public RiskGate {
 public:
  [[nodiscard]] RiskDecision decide(const NewOrderCommand& /*command*/) const override {
    return RiskDecision{
        .verdict = RiskVerdict::kApprove, .resized_quantity_units = 0, .reason = ""};
  }
};

class AlwaysRejectRiskGate final : public RiskGate {
 public:
  [[nodiscard]] RiskDecision decide(const NewOrderCommand& /*command*/) const override {
    return RiskDecision{.verdict = RiskVerdict::kReject,
                        .resized_quantity_units = 0,
                        .reason = "test: always reject"};
  }
};

class ResizeToTenRiskGate final : public RiskGate {
 public:
  [[nodiscard]] RiskDecision decide(const NewOrderCommand& /*command*/) const override {
    return RiskDecision{
        .verdict = RiskVerdict::kResize, .resized_quantity_units = 10, .reason = ""};
  }
};

TEST(OrderManager, ApprovedOrderReachesSubmittedThenAcknowledged) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);

  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_NE(tracked, nullptr);
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kSubmitted);
  EXPECT_EQ(adapter.calls_made(), 1U);  // submit() was actually called.

  manager.handle_order_accepted(OrderAcceptedEvent{.order_id = 777,
                                                   .instrument_id = 1,
                                                   .participant_id = 100,
                                                   .client_order_id = client_order_id,
                                                   .side = Side::kBuy,
                                                   .order_type = OrderType::kLimit,
                                                   .price_units = 1000,
                                                   .quantity_units = 50});
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kAcknowledged);
  EXPECT_EQ(tracked->exchange_order_id, 777U);
  ASSERT_NE(manager.find_by_exchange_order_id(777), nullptr);
}

TEST(OrderManager, RejectedRiskDecisionNeverCallsTheAdapter) {
  RecordedResponseAdapter adapter({});
  AlwaysRejectRiskGate risk;
  OrderManager manager(adapter, risk);

  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_NE(tracked, nullptr);
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kRejected);
  EXPECT_EQ(adapter.calls_made(), 0U);  // The mandatory risk seam: never sent.
}

TEST(OrderManager, ExchangeRejectionOfANewOrderTransitionsToRejected) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);

  manager.handle_order_rejected(OrderRejectedEvent{.instrument_id = 1,
                                                   .participant_id = 100,
                                                   .client_order_id = client_order_id,
                                                   .order_id = 0,
                                                   .reason = RejectReason::kPriceOutOfBand});

  EXPECT_EQ(manager.find_by_client_order_id(client_order_id)->lifecycle.state(),
            OrderState::kRejected);
}

TEST(OrderManager, ResizeVerdictSubmitsTheResizedQuantityNotTheRequested) {
  RecordedResponseAdapter adapter({});
  ResizeToTenRiskGate risk;
  OrderManager manager(adapter, risk);

  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 999);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_NE(tracked, nullptr);
  EXPECT_EQ(tracked->original_quantity_units, 10);
  EXPECT_EQ(tracked->remaining_units, 10);
}

// AEGIS-112: cancel/amend races produce deterministic outcomes.
TEST(OrderManager, RejectedCancelRevertsToAcknowledgedWhenNothingHadFilled) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});
  const auto* tracked = manager.find_by_client_order_id(client_order_id);

  ASSERT_TRUE(manager.cancel_order(client_order_id));
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kCancelPending);

  manager.handle_order_rejected(
      OrderRejectedEvent{.order_id = 1, .reason = RejectReason::kUnknownOrderId});
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kAcknowledged);  // Race lost: still live.
}

TEST(OrderManager, RejectedCancelRevertsToPartiallyFilledWhenSomethingHadFilled) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});
  manager.handle_trade(TradeEvent{.price_units = 1000,
                                  .quantity_units = 10,
                                  .maker_order_id = 1,
                                  .taker_order_id = 999,
                                  .taker_side = Side::kSell});
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_EQ(tracked->lifecycle.state(), OrderState::kPartiallyFilled);

  ASSERT_TRUE(manager.cancel_order(client_order_id));
  manager.handle_order_rejected(
      OrderRejectedEvent{.order_id = 1, .reason = RejectReason::kUnknownOrderId});
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kPartiallyFilled);  // Not kAcknowledged.
}

TEST(OrderManager, FillRacingAnInFlightCancelLandsBeforeTheCancelAcknowledgement) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});
  const auto* tracked = manager.find_by_client_order_id(client_order_id);

  ASSERT_TRUE(manager.cancel_order(client_order_id));
  ASSERT_EQ(tracked->lifecycle.state(), OrderState::kCancelPending);

  // The fill wins the race: it lands while the cancel is still in flight.
  manager.handle_trade(TradeEvent{.price_units = 1000,
                                  .quantity_units = 50,
                                  .maker_order_id = 1,
                                  .taker_order_id = 999,
                                  .taker_side = Side::kSell});
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kPartiallyFilled);
  manager.handle_order_terminated(
      OrderTerminatedEvent{.order_id = 1, .reason = TerminationReason::kFilled});
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kFilled);
  EXPECT_EQ(tracked->cumulative_filled_units, 50);
  EXPECT_EQ(tracked->remaining_units, 0);
}

TEST(OrderManager, CancelIsRefusedWithoutAnAcknowledgedExchangeOrderId) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  // Never acknowledged: exchange_order_id is still 0.
  EXPECT_FALSE(manager.cancel_order(client_order_id));
  EXPECT_EQ(adapter.calls_made(), 1U);  // Only the original submit -- cancel was never sent.
}

// AEGIS-114: partial fills.
TEST(OrderManager, PartialFillsAccumulateCorrectlyAndTerminationCompletesTheOrder) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 100);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});
  const auto* tracked = manager.find_by_client_order_id(client_order_id);

  manager.handle_trade(TradeEvent{.price_units = 1000,
                                  .quantity_units = 30,
                                  .maker_order_id = 1,
                                  .taker_order_id = 2,
                                  .taker_side = Side::kSell});
  EXPECT_EQ(tracked->cumulative_filled_units, 30);
  EXPECT_EQ(tracked->remaining_units, 70);
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kPartiallyFilled);

  manager.handle_trade(TradeEvent{.price_units = 1000,
                                  .quantity_units = 70,
                                  .maker_order_id = 1,
                                  .taker_order_id = 3,
                                  .taker_side = Side::kSell});
  EXPECT_EQ(tracked->cumulative_filled_units, 100);
  EXPECT_EQ(tracked->remaining_units, 0);
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kPartiallyFilled);  // Not kFilled yet.

  manager.handle_order_terminated(
      OrderTerminatedEvent{.order_id = 1, .reason = TerminationReason::kFilled});
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kFilled);
}

TEST(OrderManager, CancellationTerminatesToCancelled) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});
  ASSERT_TRUE(manager.cancel_order(client_order_id));
  manager.handle_order_terminated(
      OrderTerminatedEvent{.order_id = 1, .reason = TerminationReason::kCanceled});
  EXPECT_EQ(manager.find_by_client_order_id(client_order_id)->lifecycle.state(),
            OrderState::kCancelled);
}

}  // namespace
