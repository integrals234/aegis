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

// ---------------------------------------------------------------------------
// AEGIS-116 and AEGIS-117 through the OMS itself.
//
// The M3 closure audit found FeeSchedule/compute_slippage and
// MissedTradeTracker built and unit-tested but never called from production
// code -- so the acceptance criteria were satisfied by the classes in
// isolation rather than by the system the criteria describe. These cases
// exercise them where they now actually live: inside OrderManager's fill and
// termination paths.
// ---------------------------------------------------------------------------

TEST(OrderManager, FillsAccrueFeesAtTheConfiguredRate) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  // 1000 ppm == 0.1% of notional.
  OrderManager manager(adapter, risk, aegis::participant::oms::FeeSchedule{.fee_rate_ppm = 1000});
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 100);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});

  manager.handle_trade(TradeEvent{.price_units = 1000,
                                  .quantity_units = 100,
                                  .maker_order_id = 1,
                                  .taker_order_id = 2,
                                  .taker_side = Side::kSell});

  // 1000 * 100 * 1000 / 1'000'000 == 100.
  EXPECT_EQ(manager.find_by_client_order_id(client_order_id)->cumulative_fees_units, 100);
  EXPECT_EQ(manager.total_fees_units(), 100);
}

TEST(OrderManager, AdverseFillPriceAccruesSlippageAgainstTheOrdersOwnPrice) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  // A buy limit at 1000 that fills at 1010 paid 10 per unit more than its
  // own reference price: adverse.
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});

  manager.handle_trade(TradeEvent{.price_units = 1010,
                                  .quantity_units = 50,
                                  .maker_order_id = 1,
                                  .taker_order_id = 2,
                                  .taker_side = Side::kSell});

  EXPECT_EQ(manager.total_slippage_cost_units(), 10 * 50);
}

TEST(OrderManager, MarketOrdersAccrueNoSlippageBecauseTheyCarryNoReferencePrice) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kMarket, 0, 50);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});

  manager.handle_trade(TradeEvent{.price_units = 1010,
                                  .quantity_units = 50,
                                  .maker_order_id = 1,
                                  .taker_order_id = 2,
                                  .taker_side = Side::kSell});

  EXPECT_EQ(manager.total_slippage_cost_units(), 0);
}

TEST(OrderManager, CancelledOrderRecordsItsUntradedRemainderAsAMissedTrade) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 100);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});
  manager.handle_trade(TradeEvent{.price_units = 1000,
                                  .quantity_units = 40,
                                  .maker_order_id = 1,
                                  .taker_order_id = 2,
                                  .taker_side = Side::kSell});
  ASSERT_TRUE(manager.cancel_order(client_order_id));

  EXPECT_EQ(manager.missed_trades().total_missed_quantity_units(), 0);  // Nothing yet.
  manager.handle_order_terminated(
      OrderTerminatedEvent{.order_id = 1, .reason = TerminationReason::kCanceled});

  EXPECT_EQ(manager.missed_trades().missed_order_count(), 1U);
  EXPECT_EQ(manager.missed_trades().total_missed_quantity_units(),
            60);  // 100 requested, 40 filled.
}

TEST(OrderManager, FullyFilledOrderRecordsNoMissedTrade) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});
  manager.handle_trade(TradeEvent{.price_units = 1000,
                                  .quantity_units = 50,
                                  .maker_order_id = 1,
                                  .taker_order_id = 2,
                                  .taker_side = Side::kSell});
  manager.handle_order_terminated(
      OrderTerminatedEvent{.order_id = 1, .reason = TerminationReason::kFilled});

  EXPECT_EQ(manager.missed_trades().missed_order_count(), 0U);
  EXPECT_EQ(manager.missed_trades().total_missed_quantity_units(), 0);
}

TEST(OrderManager, ExchangeRejectedOrderRecordsItsWholeSizeAsMissed) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 75);

  manager.handle_order_rejected(OrderRejectedEvent{.instrument_id = 1,
                                                   .participant_id = 100,
                                                   .client_order_id = client_order_id,
                                                   .order_id = 0,
                                                   .reason = RejectReason::kPriceOutOfBand});

  EXPECT_EQ(manager.missed_trades().total_missed_quantity_units(), 75);
}

// AEGIS-113 through the OMS: an order carries the five-stage attribution for
// the market event that motivated it. The M3 closure audit found LatencyModel
// with no production caller at all; these cases assert the wiring.
TEST(OrderManager, SubmittedOrderCarriesAFiveStageLatencyAttribution) {
  using aegis::common::Duration;
  using aegis::common::EventTime;
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk, aegis::participant::oms::FeeSchedule{},
                       aegis::participant::oms::LatencyConfig{.feed_delay = Duration{40},
                                                              .decision_delay = Duration{100},
                                                              .gateway_delay = Duration{250},
                                                              .exchange_delay = Duration{700},
                                                              .ack_delay = Duration{4000}});
  ASSERT_TRUE(manager.models_latency());

  const auto client_order_id = manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000,
                                                        50, EventTime{1'000'000});
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_NE(tracked, nullptr);
  ASSERT_TRUE(tracked->latency.has_value());

  EXPECT_EQ(tracked->latency.value_or({}).feed_latency(), Duration{40});
  EXPECT_EQ(tracked->latency.value_or({}).gateway_latency(), Duration{250});
  EXPECT_EQ(tracked->latency.value_or({}).exchange_latency(), Duration{700});
  EXPECT_EQ(tracked->latency.value_or({}).total_latency(), Duration{5090});
  EXPECT_TRUE(tracked->latency.value_or({}).reconciles());
}

TEST(OrderManager, WithoutALatencyModelNoAttributionIsFabricated) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  EXPECT_FALSE(manager.models_latency());

  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  // No model configured: no attribution recorded at all, rather than a
  // zero-latency one that would read as a measurement.
  EXPECT_FALSE(manager.find_by_client_order_id(client_order_id)->latency.has_value());
}

// AEGIS-117's second half: the frozen description names opportunity cost, not
// just the missed quantity. Cost is measured against a mark the CALLER
// supplies -- the OMS observes no market and will not invent one.
TEST(OrderManager, MissedTradeOpportunityCostIsMeasuredAgainstACallerSuppliedMark) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  const auto client_order_id =
      manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 100);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 1, .participant_id = 100, .client_order_id = client_order_id});
  manager.handle_trade(TradeEvent{.price_units = 1000,
                                  .quantity_units = 40,
                                  .maker_order_id = 1,
                                  .taker_order_id = 2,
                                  .taker_side = Side::kSell});
  ASSERT_TRUE(manager.cancel_order(client_order_id));
  manager.handle_order_terminated(
      OrderTerminatedEvent{.order_id = 1, .reason = TerminationReason::kCanceled});

  ASSERT_EQ(manager.missed_trades().total_missed_quantity_units(), 60);

  // The market rose to 1050 after the buy at 1000 was cancelled: missing the
  // remaining 60 units cost 50/unit.
  EXPECT_EQ(manager.missed_trades().total_opportunity_cost_units(1050), 50 * 60);
  // Had it fallen instead, missing the order was favourable -- a negative cost.
  EXPECT_EQ(manager.missed_trades().total_opportunity_cost_units(980), -20 * 60);
  // At the order's own price, missing it cost nothing.
  EXPECT_EQ(manager.missed_trades().total_opportunity_cost_units(1000), 0);
}

}  // namespace
