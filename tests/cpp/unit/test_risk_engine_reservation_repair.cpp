#include <gtest/gtest.h>

#include "cpp/participant/risk/risk_engine.hpp"
#include "tests/cpp/support/risk_seam_test_helpers.hpp"

/// M5 closure repair: dedicated regression coverage for the three blockers
/// an independent risk-safety review found in the ORIGINAL split (reserve at
/// decide_order, match a pending leg by (instrument, side, quantity)):
///
///   1. Two individually-safe proposals could jointly breach a cumulative
///      limit, because neither's exposure was counted until its order
///      physically reached the OMS seam.
///   2. An order could resolve to a look-alike ARMED LEG from a different
///      strategy/proposal purely by coincidence of economics, inheriting
///      that other proposal's non-halted state.
///   3. A replayed proposal_id could append a second terminal
///      ProposalRiskDecision (AEGIS-137), and an order could be audited
///      against the wrong proposal.
///
/// Each test below reproduces the ORIGINAL attack shape and asserts the
/// fixed behavior; every one of them would fail against the engine as it
/// stood before this repair.
namespace {

using aegis::common::Nanos;
using aegis::participant::risk::InstrumentInfo;
using aegis::participant::risk::OrderRequest;
using aegis::participant::risk::PositionLimit;
using aegis::participant::risk::ReasonCode;
using aegis::participant::risk::RiskEngine;
using aegis::participant::risk::RiskLimitsConfig;
using aegis::participant::risk::RiskVerdict;
using aegis::participant::risk::Side;
using aegis::testing::decide_registered_order;

constexpr std::uint32_t kNear = 3001;
constexpr std::uint32_t kFar = 3002;

RiskLimitsConfig base_config() {
  RiskLimitsConfig config;
  config.base_currency = "USD";
  config.instruments[kNear] =
      InstrumentInfo{.multiplier_units = 1, .currency = "USD", .market = "EQX", .sector = "index"};
  config.instruments[kFar] =
      InstrumentInfo{.multiplier_units = 1, .currency = "USD", .market = "EQX", .sector = "index"};
  return config;
}

void seed_valid_quote(RiskEngine& engine, std::uint32_t instrument_id, std::int64_t price,
                      Nanos observed_at_nanos = 0) {
  engine.on_market_data(instrument_id, price, observed_at_nanos, /*valid=*/true);
}

OrderRequest make_request(std::uint32_t instrument_id, Side side, std::int64_t price,
                          std::int64_t quantity, const std::string& strategy_id,
                          const std::string& proposal_id, std::uint32_t leg_index = 0,
                          Nanos now = 0) {
  return OrderRequest{.strategy_id = strategy_id,
                      .proposal_id = proposal_id,
                      .leg_index = leg_index,
                      .instrument_id = instrument_id,
                      .side = side,
                      .price_units = price,
                      .quantity_units = quantity,
                      .request_time_nanos = now};
}

// =================================================================
// ATTACK 1: concurrent proposals jointly breaching a cumulative limit
// =================================================================

TEST(ReservationRepairConcurrentProposals,
     TwoIndividuallySafeProposalsRejectAtCommitNotJustAtSeam) {
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);

  const auto& first = engine.commit_proposal_decision(
      "strat", "conc-a", {make_request(kNear, Side::kBuy, 100, 60, "strat", "conc-a")}, 0);
  ASSERT_EQ(first.verdict, RiskVerdict::kApprove);

  // Before this repair, this second commit ALSO succeeded (approve), because
  // nothing counted "conc-a"'s exposure until its order reached decide_order
  // -- the two together would then jointly reserve 120 against a 100 limit.
  // Reserving at commit time means the second proposal's OWN preflight now
  // sees the first's reservation and rejects BEFORE either order exists.
  const auto& second = engine.commit_proposal_decision(
      "strat", "conc-b", {make_request(kNear, Side::kBuy, 100, 60, "strat", "conc-b")}, 0);
  EXPECT_EQ(second.verdict, RiskVerdict::kReject);
  EXPECT_EQ(second.reason_code, ReasonCode::kMaxPositionLong);

  // conc-a's own reservation is the only one that exists.
  EXPECT_EQ(engine.state().reserved_units(kNear), 60);
  EXPECT_EQ(engine.state().leg_reservation_count(), 1U);
}

TEST(ReservationRepairConcurrentProposals, PortfolioNotionalSeesAnEarlierProposalsReservation) {
  RiskLimitsConfig config = base_config();
  config.max_portfolio_notional_units = 15'000;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);

  const auto& first = engine.commit_proposal_decision(
      "strat", "not-a", {make_request(kNear, Side::kBuy, 100, 100, "strat", "not-a")}, 0);
  ASSERT_EQ(first.verdict, RiskVerdict::kApprove);  // 100 * 100 = 10,000 <= 15,000.

  // A second proposal that would itself be within the limit against filled
  // state alone (100 * 100 = 10,000 <= 15,000) must be rejected once the
  // first's reservation (10,000) is correctly included: 10,000 + 10,000 =
  // 20,000 > 15,000.
  const auto& second = engine.commit_proposal_decision(
      "strat", "not-b", {make_request(kNear, Side::kBuy, 100, 100, "strat", "not-b")}, 0);
  EXPECT_EQ(second.verdict, RiskVerdict::kReject);
  EXPECT_EQ(second.reason_code, ReasonCode::kMaxPortfolioNotional);
}

TEST(ReservationRepairConcurrentProposals, MarketExposureSeesAnEarlierProposalsReservation) {
  RiskLimitsConfig config = base_config();
  config.market_exposure_limit_units["EQX"] = 800;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  const auto& first = engine.commit_proposal_decision(
      "strat", "mkt-a", {make_request(kNear, Side::kBuy, 100, 5, "strat", "mkt-a")}, 0);
  ASSERT_EQ(first.verdict, RiskVerdict::kApprove);  // 500 <= 800.

  // A DIFFERENT instrument in the SAME market group ("EQX"): individually
  // 300 <= 800, but 500 (already reserved) + 300 = 800 -- exactly at the
  // limit -- and one more unit breaches it, proving the group total is read
  // live, not snapshotted at evaluation time only.
  const auto& second = engine.commit_proposal_decision(
      "strat", "mkt-b", {make_request(kFar, Side::kBuy, 100, 4, "strat", "mkt-b")}, 0);
  EXPECT_EQ(second.verdict, RiskVerdict::kReject);
  EXPECT_EQ(second.reason_code, ReasonCode::kMarketExposure);
}

TEST(ReservationRepairConcurrentProposals, MarginSeesAnEarlierProposalsReservation) {
  RiskLimitsConfig config = base_config();
  config.margin.margin_per_contract_units[kNear] = 100;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 10);
  engine.on_equity_update(1'000);

  const auto& first = engine.commit_proposal_decision(
      "strat", "mgn-a", {make_request(kNear, Side::kBuy, 10, 5, "strat", "mgn-a")}, 0);
  ASSERT_EQ(first.verdict, RiskVerdict::kApprove);  // 5 * 100 = 500 <= 1,000.

  // Individually 5 * 100 = 500 <= 1,000, but 500 (already reserved) + 500 =
  // 1,000 is the boundary; one more contract breaches it.
  const auto& second = engine.commit_proposal_decision(
      "strat", "mgn-b", {make_request(kNear, Side::kBuy, 10, 6, "strat", "mgn-b")}, 0);
  EXPECT_EQ(second.verdict, RiskVerdict::kReject);
  EXPECT_EQ(second.reason_code, ReasonCode::kInsufficientMargin);
}

// =================================================================
// ATTACK 2: look-alike leg across strategies, kill switch
// =================================================================

TEST(ReservationRepairExactIdentity, LookAlikeLegFromAnotherStrategyCannotBorrowItsRiskState) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);

  // Strategy A and strategy B each arm an ECONOMICALLY IDENTICAL leg
  // (same instrument, side, quantity) -- the exact shape a tuple-based
  // (instrument, side, quantity) lookup could not tell apart.
  const auto& decision_a = engine.commit_proposal_decision(
      "strat-a", "look-a", {make_request(kNear, Side::kBuy, 100, 10, "strat-a", "look-a")}, 0);
  ASSERT_EQ(decision_a.verdict, RiskVerdict::kApprove);
  const auto& decision_b = engine.commit_proposal_decision(
      "strat-b", "look-b", {make_request(kNear, Side::kBuy, 100, 10, "strat-b", "look-b")}, 0);
  ASSERT_EQ(decision_b.verdict, RiskVerdict::kApprove);

  ASSERT_TRUE(engine.trip_strategy("strat-b"));

  // Submit B's EXACT registered identity. It must resolve to B (not A), and
  // be rejected because B -- not A -- is halted.
  const auto b_decision = decide_registered_order(engine, "strat-b", "look-b", /*leg_index=*/0,
                                                  kNear, Side::kBuy, 10, /*client_order_id=*/2, 0);
  EXPECT_EQ(b_decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(b_decision.reason_code, ReasonCode::kKillSwitchStrategy);

  // A's own leg is completely untouched: still reservable, still resolvable.
  EXPECT_EQ(engine.state().leg_reservation_count(), 1U);  // Only A's leg remains armed.
  const auto a_decision = decide_registered_order(engine, "strat-a", "look-a", /*leg_index=*/0,
                                                  kNear, Side::kBuy, 10, /*client_order_id=*/1, 0);
  EXPECT_EQ(a_decision.verdict, RiskVerdict::kApprove);

  // The audit record for B's rejection is attributed to B, never to A.
  const auto& order_decisions = engine.audit_log().order_decisions();
  ASSERT_EQ(order_decisions.size(), 2U);
  const auto& b_record = order_decisions.front();
  EXPECT_EQ(b_record.proposal_id, "look-b");
}

// =================================================================
// ATTACK 3: duplicate proposal replay and audit mis-attribution
// =================================================================

TEST(ReservationRepairAuditIntegrity, MisattributionCannotCrossProposalIdentities) {
  // Two proposals, from two strategies, with ECONOMICALLY IDENTICAL legs.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);

  engine.commit_proposal_decision(
      "strat-a", "mis-a", {make_request(kNear, Side::kBuy, 100, 10, "strat-a", "mis-a")}, 0);
  engine.commit_proposal_decision(
      "strat-b", "mis-b", {make_request(kNear, Side::kBuy, 100, 10, "strat-b", "mis-b")}, 0);

  // Submit B first.
  const auto b_decision = decide_registered_order(engine, "strat-b", "mis-b", /*leg_index=*/0,
                                                  kNear, Side::kBuy, 10, /*client_order_id=*/20, 0);
  ASSERT_EQ(b_decision.verdict, RiskVerdict::kApprove);

  const auto& order_decisions = engine.audit_log().order_decisions();
  ASSERT_EQ(order_decisions.size(), 1U);
  EXPECT_EQ(order_decisions.front().proposal_id, "mis-b");
  EXPECT_EQ(order_decisions.front().client_order_id, 20U);
  EXPECT_EQ(engine.audit_log().proposal_decision_count("mis-a"), 1U);
  EXPECT_EQ(engine.audit_log().proposal_decision_count("mis-b"), 1U);

  // Now submit A -- reverse order, to rule out insertion-order dependence.
  const auto a_decision = decide_registered_order(engine, "strat-a", "mis-a", /*leg_index=*/0,
                                                  kNear, Side::kBuy, 10, /*client_order_id=*/10, 0);
  ASSERT_EQ(a_decision.verdict, RiskVerdict::kApprove);
  ASSERT_EQ(engine.audit_log().order_decisions().size(), 2U);
  EXPECT_EQ(engine.audit_log().order_decisions().back().proposal_id, "mis-a");
  EXPECT_EQ(engine.audit_log().order_decisions().back().client_order_id, 10U);

  // Still exactly one terminal ProposalRiskDecision each -- no event for
  // either proposal was recorded against the other.
  EXPECT_EQ(engine.audit_log().proposal_decision_count("mis-a"), 1U);
  EXPECT_EQ(engine.audit_log().proposal_decision_count("mis-b"), 1U);
}

TEST(ReservationRepairAuditIntegrity, ReplayingAProposalIdNeverAppendsASecondTerminalDecision) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);

  const std::vector<OrderRequest> legs{make_request(kNear, Side::kBuy, 100, 10, "strat", "rep-1")};
  const auto& first = engine.commit_proposal_decision("strat", "rep-1", legs, 0);
  ASSERT_EQ(first.verdict, RiskVerdict::kApprove);

  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto& replay = engine.commit_proposal_decision("strat", "rep-1", legs, attempt + 1);
    EXPECT_EQ(replay.sequence, first.sequence);
  }
  EXPECT_EQ(engine.audit_log().proposal_decision_count("rep-1"), 1U);
  // No extra capacity was consumed by the replays: still exactly one leg
  // reservation, for the original commit.
  EXPECT_EQ(engine.state().leg_reservation_count(), 1U);
}

// =================================================================
// Out-of-band state change between commit and seam arrival
// =================================================================

TEST(ReservationRepairSeamRevalidation, KillSwitchTrippedAfterCommitStopsTheOrderAtTheSeam) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);

  const auto& decision = engine.commit_proposal_decision(
      "strat", "revalid-kill", {make_request(kNear, Side::kBuy, 100, 10, "strat", "revalid-kill")},
      0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);

  // Out-of-band: the strategy is halted AFTER commit, BEFORE the order
  // reaches the seam.
  ASSERT_TRUE(engine.trip_strategy("strat"));

  const auto oms_decision =
      decide_registered_order(engine, "strat", "revalid-kill", /*leg_index=*/0, kNear, Side::kBuy,
                              10, /*client_order_id=*/1, 0);
  EXPECT_EQ(oms_decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(oms_decision.reason_code, ReasonCode::kKillSwitchStrategy);
  // The now-rejected leg's reservation is released, not left dangling.
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
  EXPECT_EQ(engine.state().reserved_units(kNear), 0);
}

TEST(ReservationRepairSeamRevalidation,
     PositionLimitLoweredAfterCommitStopsTheOrderAtTheSeamWithoutDoubleCountingItsOwnReservation) {
  // Out-of-band exposure change: a SEPARATE proposal's fill lands between
  // this proposal's commit and its own order reaching the seam, pushing the
  // projected position (INCLUDING this leg's own reservation) over the
  // limit. revalidate_at_seam must catch this -- but must also not
  // double-count this leg's own already-reserved contribution (which would
  // reject spuriously even with no out-of-band change at all).
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);

  const auto& decision = engine.commit_proposal_decision(
      "strat", "revalid-pos", {make_request(kNear, Side::kBuy, 100, 60, "strat", "revalid-pos")},
      0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);  // 60 <= 100.

  // A confirmed fill from some OTHER, already-terminal order lands directly
  // on the books (not modelled through this engine's own commit path, since
  // that would itself be blocked by the same limit -- this simulates state
  // the composition root feeds from Portfolio/fills outside this proposal's
  // own lifecycle).
  engine.on_fill(/*client_order_id=*/999, kNear, Side::kBuy, 50);

  // Now: confirmed position (50) + this leg's own reservation (60) = 110 >
  // 100. The seam must catch this even though nothing was wrong AT COMMIT
  // time.
  const auto oms_decision =
      decide_registered_order(engine, "strat", "revalid-pos", /*leg_index=*/0, kNear, Side::kBuy,
                              60, /*client_order_id=*/1, 0);
  EXPECT_EQ(oms_decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(oms_decision.reason_code, ReasonCode::kMaxPositionLong);
}

TEST(ReservationRepairSeamRevalidation, NoDoubleCountingMeansAnUnchangedSafeProposalStillApproves) {
  // The negative control for the test above: with NO out-of-band change at
  // all, a proposal that was safe at commit time must STILL be safe at the
  // seam -- proving revalidate_at_seam's overlay-subtraction correctly
  // excludes this leg's own reservation rather than counting it twice.
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);

  const auto& decision = engine.commit_proposal_decision(
      "strat", "no-double-count",
      {make_request(kNear, Side::kBuy, 100, 100, "strat", "no-double-count")}, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);  // Exactly at the limit.

  const auto oms_decision = decide_registered_order(engine, "strat", "no-double-count",
                                                    /*leg_index=*/0, kNear, Side::kBuy, 100,
                                                    /*client_order_id=*/1, 0);
  EXPECT_EQ(oms_decision.verdict, RiskVerdict::kApprove);
}

// =================================================================
// Two-leg atomicity: reservation, not just audit legs, is all-or-nothing
// =================================================================

TEST(ReservationRepairAtomicity, RejectedProposalArmsAndReservesNothingForEitherLeg) {
  RiskLimitsConfig config = base_config();
  config.order_quantity_limits[kFar] = aegis::participant::risk::OrderQuantityLimit{
      .max_order_quantity_units = 0, .resize_on_breach = false};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kSell, 100, 5, "spread", "atomic-1", 0),
      make_request(kFar, Side::kBuy, 100, 5, "spread", "atomic-1", 1),  // Rejects: qty cap 0.
  };
  const auto& decision = engine.commit_proposal_decision("spread", "atomic-1", legs, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kReject);

  // Neither leg -- not even the near leg, which was individually fine --
  // was armed or reserved.
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
  EXPECT_EQ(engine.state().reserved_units(kNear), 0);
  EXPECT_EQ(engine.state().reserved_units(kFar), 0);

  // Confirming no leg was armed: an order for the (individually-fine) near
  // leg is rejected as unexpected, not approved.
  const auto oms_decision =
      decide_registered_order(engine, "spread", "atomic-1", /*leg_index=*/0, kNear, Side::kSell, 5,
                              /*client_order_id=*/1, 0);
  EXPECT_EQ(oms_decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(oms_decision.reason_code, ReasonCode::kUnexpectedOrder);
}

// =================================================================
// Identity/economics mismatch (a caller bug, never a look-alike match)
// =================================================================

TEST(ReservationRepairExactIdentity, RegisteredIdentityWithDisagreeingEconomicsIsRejected) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);

  const auto& decision = engine.commit_proposal_decision(
      "strat", "mismatch-1", {make_request(kNear, Side::kBuy, 100, 10, "strat", "mismatch-1")}, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);

  engine.register_pending_order_identity(/*client_order_id=*/1, "strat", "mismatch-1",
                                         /*leg_index=*/0);
  // The registered identity is correct, but the order itself carries a
  // DIFFERENT quantity than what was actually reserved.
  const auto oms_decision = engine.decide_order(kNear, Side::kBuy, /*quantity_units=*/999,
                                                /*client_order_id=*/1, 0);
  EXPECT_EQ(oms_decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(oms_decision.reason_code, ReasonCode::kIdentityMismatch);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);  // Released, not left dangling.
}

}  // namespace
