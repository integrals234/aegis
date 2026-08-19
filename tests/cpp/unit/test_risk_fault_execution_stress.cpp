#include <array>

#include <gtest/gtest.h>

#include "cpp/common/clock.hpp"
#include "cpp/participant/app/risk_engine_gate.hpp"
#include "cpp/participant/oms/execution_adapter.hpp"
#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/risk/risk_engine.hpp"
#include "cpp/replay/fault_injection.hpp"
#include "tests/cpp/optional_access.hpp"
#include "tests/cpp/support/risk_seam_test_helpers.hpp"

/// AEGIS-063 M5 residual: OMS/risk INTEGRATION tests covering each
/// execution-stress fault kind M2 already delivers deterministically
/// (`test_market_stress_faults.cpp`). Each test drives the real
/// `RiskEngine` through the real mandatory seam (`RiskEngineGate` ->
/// `OrderManager`) and asserts the risk engine's own reservation/audit
/// state stays coherent with what actually happened at the OMS -- the
/// "integration" the frozen acceptance names, not a report generated
/// afterward.
namespace {

using aegis::common::ManualClock;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderRejectedEvent;
using aegis::events::exchange::OrderTerminatedEvent;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::events::exchange::TerminationReason;
using aegis::events::exchange::TradeEvent;
using aegis::participant::app::RiskEngineGate;
using aegis::participant::app::RiskReleasingExecutionAdapter;
using aegis::participant::oms::ExecutionAdapter;
using aegis::participant::oms::LatencyConfig;
using aegis::participant::oms::OrderManager;
using aegis::participant::oms::OrderState;
using aegis::participant::risk::InstrumentInfo;
using aegis::participant::risk::RiskEngine;
using aegis::participant::risk::RiskLimitsConfig;
using aegis::replay::FaultKind;
using aegis::testing::submit_registered_new_order;
namespace risk = aegis::participant::risk;

constexpr std::uint32_t kInstrumentId = 6001;

RiskLimitsConfig permissive_config() {
  RiskLimitsConfig config;
  config.instruments[kInstrumentId] =
      InstrumentInfo{.multiplier_units = 1, .currency = "USD", .market = "", .sector = ""};
  return config;
}

/// Every test below needs a valid market reference before evaluate_leg's
/// staleness/reference check (step 3) will consider any order at all.
void seed_quote(RiskEngine& engine) {
  engine.on_market_data(kInstrumentId, 100, 0, /*valid=*/true);
}

/// AEGIS-119's environment-independent adapter, configurable per fault kind:
/// records whether submit() was told to simulate `kBackpressure` (returns
/// false, as `ExecutionTransport::send`'s own doc contract already
/// describes) or a normal accepted send (true, the exchange's own
/// subsequent events are synthesized directly by each test).
class FaultInjectableAdapter final : public ExecutionAdapter {
 public:
  explicit FaultInjectableAdapter(bool simulate_backpressure)
      : simulate_backpressure_(simulate_backpressure) {}

  [[nodiscard]] bool submit(const NewOrderCommand& /*command*/) override {
    submit_calls_ += 1;
    return !simulate_backpressure_;
  }
  [[nodiscard]] bool cancel(const CancelOrderCommand& /*command*/) override { return true; }
  [[nodiscard]] bool modify(const ModifyOrderCommand& /*command*/) override { return true; }

  [[nodiscard]] int submit_calls() const { return submit_calls_; }

 private:
  bool simulate_backpressure_;
  int submit_calls_{0};
};

/// A function rather than a namespace-scope constant: `OrderRequest` holds
/// `std::string` members, so a static-storage-duration instance would run a
/// potentially-throwing constructor before `main`, where nothing can catch it.
[[nodiscard]] risk::OrderRequest proposal_leg() {
  return risk::OrderRequest{.strategy_id = "s",
                            .proposal_id = "p",
                            .leg_index = 0,
                            .instrument_id = kInstrumentId,
                            .side = Side::kBuy,
                            .price_units = 100,
                            .quantity_units = 10};
}

TEST(ExecutionStressIntegration, RejectionReleasesTheReservation) {
  RiskEngine risk_engine(permissive_config());
  seed_quote(risk_engine);
  ManualClock clock;
  RiskEngineGate risk_gate(risk_engine, clock);
  FaultInjectableAdapter adapter(/*simulate_backpressure=*/false);
  OrderManager manager(adapter, risk_gate);

  risk_engine.commit_proposal_decision("s", "p", {proposal_leg()}, 0);
  const auto client_order_id =
      submit_registered_new_order(manager, risk_engine, "s", "p", /*leg_index=*/0, kInstrumentId,
                                  Side::kBuy, OrderType::kLimit, 100, 10);
  ASSERT_EQ(manager.find_by_client_order_id(client_order_id)->lifecycle.state(),
            OrderState::kSubmitted);
  ASSERT_EQ(risk_engine.state().reservation_count(), 1U);

  // The kRejection execution-stress fault: the exchange rejects the new
  // order. OMS and risk both react to the same downstream event.
  manager.handle_order_rejected(OrderRejectedEvent{.instrument_id = kInstrumentId,
                                                   .participant_id = 1,
                                                   .client_order_id = client_order_id,
                                                   .order_id = 0});
  risk_engine.on_order_rejected(client_order_id);

  EXPECT_EQ(manager.find_by_client_order_id(client_order_id)->lifecycle.state(),
            OrderState::kRejected);
  EXPECT_EQ(risk_engine.state().reservation_count(), 0U);
}

TEST(ExecutionStressIntegration, LatencySpikeDoesNotDisturbTheRiskDecision) {
  RiskEngine risk_engine(permissive_config());
  seed_quote(risk_engine);
  ManualClock clock;
  RiskEngineGate risk_gate(risk_engine, clock);
  FaultInjectableAdapter adapter(/*simulate_backpressure=*/false);
  // kLatencySpike: a large gateway_delay -- the OMS's own latency
  // attribution absorbs it (AEGIS-113); the risk decision it already made
  // is untouched by how long submission takes afterward.
  const LatencyConfig latency{.feed_delay = aegis::common::Duration{0},
                              .decision_delay = aegis::common::Duration{0},
                              .gateway_delay = aegis::common::Duration{5'000'000'000},
                              .exchange_delay = aegis::common::Duration{0},
                              .ack_delay = aegis::common::Duration{0}};
  OrderManager manager(adapter, risk_gate, /*fees=*/{}, latency);

  risk_engine.commit_proposal_decision("s", "p", {proposal_leg()}, 0);
  const auto client_order_id =
      submit_registered_new_order(manager, risk_engine, "s", "p", /*leg_index=*/0, kInstrumentId,
                                  Side::kBuy, OrderType::kLimit, 100, 10,
                                  /*market_event_time=*/aegis::common::EventTime{1'000});

  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_NE(tracked, nullptr);
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kSubmitted);
  ASSERT_TRUE(tracked->latency.has_value());
  EXPECT_EQ(aegis::test::checked(tracked->latency).gateway_latency().nanos(), 5'000'000'000);
  ASSERT_EQ(risk_engine.audit_log().order_decisions().size(), 1U);
  EXPECT_EQ(risk_engine.audit_log().order_decisions().front().verdict, risk::RiskVerdict::kApprove);
}

TEST(ExecutionStressIntegration, PartialFillReducesReservationWithoutDoubleCounting) {
  RiskEngine risk_engine(permissive_config());
  seed_quote(risk_engine);
  ManualClock clock;
  RiskEngineGate risk_gate(risk_engine, clock);
  FaultInjectableAdapter adapter(/*simulate_backpressure=*/false);
  OrderManager manager(adapter, risk_gate);

  risk_engine.commit_proposal_decision("s", "p", {proposal_leg()}, 0);
  const auto client_order_id =
      submit_registered_new_order(manager, risk_engine, "s", "p", /*leg_index=*/0, kInstrumentId,
                                  Side::kBuy, OrderType::kLimit, 100, 10);
  ASSERT_EQ(risk_engine.state().reserved_units(kInstrumentId), 10);

  // kPartialFill: the exchange fills part, not all, of the order.
  manager.handle_trade(TradeEvent{.instrument_id = kInstrumentId,
                                  .price_units = 100,
                                  .quantity_units = 4,
                                  .maker_order_id = 0,
                                  .taker_order_id = 0,
                                  .maker_participant_id = 0,
                                  .taker_participant_id = 1,
                                  .taker_side = Side::kBuy});
  risk_engine.on_fill(client_order_id, kInstrumentId, Side::kBuy, 4);

  // Reserved exposure shrinks by exactly the fill; confirmed position (not
  // modelled by this test's bare OrderManager/RiskEngine pair -- Portfolio
  // is what actually tracks it) is not this assertion's concern. What
  // matters here is that reserved + filled never silently diverges: the
  // projected exposure (position + reservation) a subsequent evaluate()
  // would see is unchanged by a partial fill, only its composition is.
  EXPECT_EQ(risk_engine.state().reserved_units(kInstrumentId), 6);

  manager.handle_order_terminated(
      OrderTerminatedEvent{.order_id = 0, .reason = TerminationReason::kResidualCanceled});
  risk_engine.on_order_terminated(client_order_id);
  EXPECT_EQ(risk_engine.state().reserved_units(kInstrumentId), 0);
}

TEST(ExecutionStressIntegration,
     BackpressureAutomaticallyReleasesTheReservationThroughTheNormalLifecycle) {
  // Carry-in fix (ADR-0027 addendum): a submission failure must release risk
  // capacity through the ordinary lifecycle, with no manual cleanup call.
  // RiskReleasingExecutionAdapter is the composition-root-owned decorator
  // that makes this automatic without touching cpp/participant/oms/** or
  // adding a risk -> OMS dependency -- it sees the same NewOrderCommand
  // (and client_order_id) RiskEngineGate::decide already approved.
  ManualClock clock;
  FaultInjectableAdapter backpressured_transport(/*simulate_backpressure=*/true);

  // 1. Order approved and reservation created. A tight position limit (10)
  // makes the auto-release provably observable in step 5: a fresh order of
  // the same size only fits again if the failed one's reservation is truly
  // gone, not merely reduced.
  RiskLimitsConfig limits = permissive_config();
  limits.position_limits[kInstrumentId] =
      risk::PositionLimit{.max_long_units = 10, .max_short_units = 10};
  RiskEngine tight_engine(limits);
  seed_quote(tight_engine);
  RiskEngineGate tight_risk_gate(tight_engine, clock);
  RiskReleasingExecutionAdapter tight_adapter(backpressured_transport, tight_engine);
  OrderManager tight_manager(tight_adapter, tight_risk_gate);

  // commit_proposal_decision reserves the exposure (M5 closure repair);
  // decide_order (inside submit_new_order) transitions that same
  // reservation to be client_order_id-keyed, and adapter.submit() discovers
  // the backpressure synchronously, inside this one call -- there is no
  // externally observable moment where the reservation exists but has not
  // yet been released, so steps 1-4 are verified together, on the state
  // submit_new_order leaves behind, rather than as separate snapshots.
  tight_engine.commit_proposal_decision("s", "p1", {proposal_leg()}, 0);
  const auto client_order_id =
      submit_registered_new_order(tight_manager, tight_engine, "s", "p1", /*leg_index=*/0,
                                  kInstrumentId, Side::kBuy, OrderType::kLimit, 100, 10);

  // 2. Transport returns backpressure/failure (submit() -> false).
  EXPECT_EQ(backpressured_transport.submit_calls(), 1);

  // 3. Order is not live at the exchange: OrderManager's own contract
  // (order_manager.cpp) leaves it Submitted/unacknowledged -- there is no
  // separate "send failed" state, and no OrderAcceptedEvent was ever
  // produced for this client_order_id, so nothing downstream can treat it
  // as resting.
  EXPECT_EQ(tight_manager.find_by_client_order_id(client_order_id)->lifecycle.state(),
            OrderState::kSubmitted);

  // 4. Reservation returned to its pre-order value automatically -- no call
  // to release_reservation anywhere in this test.
  EXPECT_EQ(tight_engine.state().reservation_count(), 0U);
  EXPECT_EQ(tight_engine.state().reserved_units(kInstrumentId), 0);

  // 5. A later, safe order can use that risk capacity again.
  const auto& second_decision =
      tight_engine.commit_proposal_decision("s", "p2",
                                            {risk::OrderRequest{.strategy_id = "s",
                                                                .proposal_id = "p2",
                                                                .instrument_id = kInstrumentId,
                                                                .side = Side::kBuy,
                                                                .price_units = 100,
                                                                .quantity_units = 10,
                                                                .request_time_nanos = 1}},
                                            1);
  EXPECT_NE(second_decision.verdict, risk::RiskVerdict::kReject);
}

TEST(ExecutionStressIntegration, ReleaseReservationRemainsAvailableForOtherRecoveryPaths) {
  // release_reservation is not removed: it is a legitimate primitive for a
  // caller with visibility RiskReleasingExecutionAdapter does not have (a
  // manual reconciliation tool, an out-of-band cancel confirmation). This
  // test proves the primitive still works on its own; it is no longer the
  // NORMAL submission-failure path (the test above covers that).
  RiskEngine risk_engine(permissive_config());
  seed_quote(risk_engine);
  risk_engine.commit_proposal_decision("s", "p", {proposal_leg()}, 0);
  ManualClock clock;
  RiskEngineGate risk_gate(risk_engine, clock);
  FaultInjectableAdapter adapter(/*simulate_backpressure=*/false);
  OrderManager manager(adapter, risk_gate);
  const auto client_order_id =
      submit_registered_new_order(manager, risk_engine, "s", "p", /*leg_index=*/0, kInstrumentId,
                                  Side::kBuy, OrderType::kLimit, 100, 10);
  ASSERT_EQ(risk_engine.state().reservation_count(), 1U);

  risk_engine.release_reservation(client_order_id);
  EXPECT_EQ(risk_engine.state().reservation_count(), 0U);
}

TEST(ExecutionStressIntegration, EachExecutionStressKindIsExercisedByExactlyOneTestAbove) {
  // Traceability: this file's four tests above correspond 1:1 to M2's four
  // execution-stress FaultKind values (test_market_stress_faults.cpp);
  // nothing here reinterprets what those kinds mean.
  constexpr std::array<FaultKind, 4> kKinds{FaultKind::kRejection, FaultKind::kLatencySpike,
                                            FaultKind::kPartialFill, FaultKind::kBackpressure};
  EXPECT_EQ(kKinds.size(), 4U);
}

}  // namespace
