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
constexpr std::uint32_t kBg = 3003;  // A third instrument, used only to dilute portfolio_notional.

RiskLimitsConfig base_config() {
  RiskLimitsConfig config;
  config.base_currency = "USD";
  config.instruments[kNear] =
      InstrumentInfo{.multiplier_units = 1, .currency = "USD", .market = "EQX", .sector = "index"};
  config.instruments[kFar] =
      InstrumentInfo{.multiplier_units = 1, .currency = "USD", .market = "EQX", .sector = "index"};
  return config;
}

// Adds a third instrument (kBg) to base_config() -- concentration is a SHARE
// of the whole portfolio, so a meaningful concentration test needs exposure
// to dilute against, not just the one or two instruments a proposal itself
// touches.
RiskLimitsConfig concentration_config(double max_concentration_share) {
  RiskLimitsConfig config = base_config();
  config.instruments[kBg] =
      InstrumentInfo{.multiplier_units = 1, .currency = "USD", .market = "EQX", .sector = "index"};
  config.concentration.max_concentration_share = max_concentration_share;
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

TEST(ReservationRepairExactIdentity, StagedIdentityWithDisagreeingEconomicsIsRejected) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);

  const auto& decision = engine.commit_proposal_decision(
      "strat", "mismatch-1", {make_request(kNear, Side::kBuy, 100, 10, "strat", "mismatch-1")}, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);

  // The staged identity names the right leg, but its economics disagree with
  // what the proposal actually committed. Since ALL constituents are checked
  // at the release epoch (before anything is executable), this is caught
  // there rather than leg-by-leg at the seam.
  aegis::testing::stage_and_authorize(
      engine, "strat", "mismatch-1",
      {aegis::testing::staged_leg(/*client_order_id=*/1, /*leg_index=*/0, kNear, Side::kBuy,
                                  /*quantity_units=*/999)});
  const auto oms_decision = engine.decide_order(kNear, Side::kBuy, /*quantity_units=*/999,
                                                /*client_order_id=*/1, 0);
  EXPECT_EQ(oms_decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(oms_decision.reason_code, ReasonCode::kIdentityMismatch);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);  // Released, not left dangling.
}

// =================================================================
// Concentration overlay accounting (repair of the blocker the risk-safety
// re-review found in THIS repair): check_concentration used to compute its
// numerator directly from state_.reserved_units + signed_candidate instead
// of through the SAME EvaluationOverlay/group_gross_notional_units every
// other cumulative control (position, notional, market/sector, correlated
// groups, margin, leverage) already uses. That caused two distinct defects:
// seam double-counting (this leg's own already-committed reservation was
// counted, then the candidate was added AGAIN) and preflight under-counting
// (a same-instrument sibling leg's staged exposure, carried in the overlay
// parameter, was never read at all). All accounting below uses literal,
// hand-checkable numbers.
// =================================================================

TEST(ConcentrationOverlayAccounting, ExactBoundaryApprovesOneUnitOverRejects) {
  // Background: 700 units of kBg filled @ 100 = 70,000 notional.
  // Proposal: 300 units of kNear @ 100 = 30,000 notional.
  // Total = 100,000. kNear's share = 30,000 / 100,000 = 0.30 EXACTLY --
  // the configured limit itself, which the documented policy (">", not
  // ">=") treats as still safe.
  RiskEngine engine(concentration_config(0.30));
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kBg, 100);
  engine.on_fill(/*client_order_id=*/900, kBg, Side::kBuy, 700);

  const auto& at_boundary = engine.commit_proposal_decision(
      "strat", "conc-boundary",
      {make_request(kNear, Side::kBuy, 100, 300, "strat", "conc-boundary")}, 0);
  EXPECT_EQ(at_boundary.verdict, RiskVerdict::kApprove);

  // One more unit of quantity: 301 * 100 = 30,100; total = 100,100; share =
  // 30,100 / 100,100 = 0.300699... > 0.30 -- must now reject.
  const auto& over_boundary = engine.commit_proposal_decision(
      "strat", "conc-over", {make_request(kNear, Side::kBuy, 100, 301, "strat", "conc-over")}, 0);
  EXPECT_EQ(over_boundary.verdict, RiskVerdict::kReject);
  EXPECT_EQ(over_boundary.reason_code, ReasonCode::kConcentration);
}

TEST(ConcentrationOverlayAccounting,
     SeamRevalidationDoesNotDoubleCountItsOwnReservationAndBothLegsOfASafeProposalSurvive) {
  // The reviewer's exact reproduction shape: a two-leg proposal whose TRUE
  // combined concentration is safe must have BOTH legs survive seam
  // revalidation -- neither may be spuriously rejected because its own
  // already-committed reservation got counted twice.
  //
  // Background: 400 units of kBg filled @ 100 = 40,000.
  // Leg 0 (near): 100 units @ 100 = 10,000.
  // Leg 1 (far):  150 units @ 100 = 15,000.
  // Total = 65,000. near share = 10,000/65,000 = 0.1538..., far share =
  // 15,000/65,000 = 0.2307... Both comfortably under the 0.30 limit.
  //
  // Before this repair: at the seam, near's stale computation would have
  // been reserved_units(100, already reserved at commit) + candidate(100
  // again) = 200 units = 20,000 notional; share = 20,000/65,000 = 0.3077 >
  // 0.30 -- a SPURIOUS reject of the near leg while far still approves,
  // exactly the naked-single-leg hazard this test proves is now closed.
  RiskEngine engine(concentration_config(0.30));
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);
  seed_valid_quote(engine, kBg, 100);
  engine.on_fill(/*client_order_id=*/900, kBg, Side::kBuy, 400);

  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kBuy, 100, 100, "strat", "conc-both-legs", 0),
      make_request(kFar, Side::kBuy, 100, 150, "strat", "conc-both-legs", 1),
  };
  const auto& decision = engine.commit_proposal_decision("strat", "conc-both-legs", legs, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);

  ASSERT_EQ(
      aegis::testing::stage_and_authorize(engine, "strat", "conc-both-legs",
                                          {aegis::testing::staged_leg(1, 0, kNear, Side::kBuy, 100),
                                           aegis::testing::staged_leg(2, 1, kFar, Side::kBuy, 150)})
          .state,
      aegis::participant::risk::ProposalReleaseState::kAuthorizedForRelease);
  const auto near_decision = engine.decide_order(kNear, Side::kBuy, 100, /*client_order_id=*/1, 0);
  const auto far_decision = engine.decide_order(kFar, Side::kBuy, 150, /*client_order_id=*/2, 0);
  EXPECT_EQ(near_decision.verdict, RiskVerdict::kApprove);
  EXPECT_EQ(far_decision.verdict, RiskVerdict::kApprove);
}

TEST(ConcentrationOverlayAccounting, PreflightSameInstrumentStagedSiblingLegIsNotUnderCounted) {
  // Two legs of ONE proposal, both on the SAME instrument -- the shape that
  // exposed the preflight half of the defect: the old numerator never read
  // the overlay a sibling leg stages, only state_.reserved_units (which is
  // zero for both legs until the WHOLE proposal is approved and committed).
  //
  // Background: 600 units of kBg filled @ 100 = 60,000.
  // Leg 0: 200 units of kNear @ 100 = 20,000. Alone: share vs (60,000 +
  //   20,000) = 20,000/80,000 = 0.25 <= 0.30 -- looks safe in isolation.
  // Leg 1: another 200 units of kNear @ 100 = 20,000, STAGED on top of leg
  //   0 via the proposal's own overlay.
  // TRUE combined kNear exposure = 400 units = 40,000; total = 100,000;
  // combined share = 40,000/100,000 = 0.40 > 0.30 -- must reject, because
  // leg 1's own evaluation must see leg 0's staged 200 units, not just its
  // own 200.
  RiskEngine engine(concentration_config(0.30));
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kBg, 100);
  engine.on_fill(/*client_order_id=*/900, kBg, Side::kBuy, 600);

  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kBuy, 100, 200, "strat", "conc-same-instrument", 0),
      make_request(kNear, Side::kBuy, 100, 200, "strat", "conc-same-instrument", 1),
  };
  const auto& decision = engine.commit_proposal_decision("strat", "conc-same-instrument", legs, 0);
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  ASSERT_EQ(decision.legs.size(), 2U);
  // Atomicity: a same-instrument sibling breach rejects the WHOLE proposal,
  // arming and reserving neither leg.
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
}

TEST(ConcentrationOverlayAccounting, ANewProposalSeesAnEarlierProposalsReservedConcentration) {
  // Background: 600 units of kBg filled @ 100 = 60,000.
  // Proposal A: 100 units of kNear @ 100 = 10,000. Alone: 10,000/70,000 =
  //   0.1428... <= 0.30 -- approves and reserves.
  // Proposal B: another 200 units of kNear @ 100 = 20,000. Individually
  //   against FILLED state alone (60,000 + 20,000 = 80,000), 20,000/80,000
  //   = 0.25 <= 0.30 would look safe -- but A's reservation (10,000) is
  //   already live: TRUE combined = 30,000/90,000 = 0.3333... > 0.30, so B
  //   must reject.
  RiskEngine engine(concentration_config(0.30));
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kBg, 100);
  engine.on_fill(/*client_order_id=*/900, kBg, Side::kBuy, 600);

  const auto& first = engine.commit_proposal_decision(
      "strat", "conc-prior-a", {make_request(kNear, Side::kBuy, 100, 100, "strat", "conc-prior-a")},
      0);
  ASSERT_EQ(first.verdict, RiskVerdict::kApprove);

  const auto& second = engine.commit_proposal_decision(
      "strat", "conc-prior-b", {make_request(kNear, Side::kBuy, 100, 200, "strat", "conc-prior-b")},
      0);
  EXPECT_EQ(second.verdict, RiskVerdict::kReject);
  EXPECT_EQ(second.reason_code, ReasonCode::kConcentration);
}

// =================================================================
// Proposal-atomic final-overlay evaluation and seam revalidation
// (repair of the SECOND blocker a risk-safety re-review found: preflight
// evaluated legs against a PREFIX overlay, so a later leg's REDUCTION of
// exposure was invisible to an earlier leg's own cumulative check, and
// decide_order revalidated each leg independently at the seam, so one
// sibling could fail while the other passed -- a naked leg either way.)
// =================================================================

TEST(ProposalAtomicSeamRevalidation, N6ExposureReductionRejectsAtomicallyAtCommit) {
  // The reviewer's exact attack shape: background exposure in B and C;
  // proposal leg 0 ADDS A exposure, leg 1 REDUCES C exposure.
  //
  // Background: B (kBg) filled 300 @ 100 = 30,000. C (kFar) filled 1,000 @
  // 100 = 100,000. Portfolio before the proposal: 130,000.
  //
  // Proposal: leg0 = A (kNear) buy 500 @ 100 = 50,000. leg1 = C sell 950,
  // reducing C's position to 50 (5,000 notional).
  //
  // TRUE final combined portfolio = B(30,000) + A(50,000) + C-after-
  // reduction(5,000) = 85,000. A's TRUE share = 50,000 / 85,000 = 0.588.
  //
  // Under the OLD prefix-only preflight, leg0 (A) was evaluated BEFORE
  // leg1's reduction was staged, so it saw a denominator of B + C-BEFORE-
  // reduction + A = 30,000 + 100,000 + 50,000 = 180,000, giving A a share
  // of 50,000/180,000 = 0.278 -- safely under a 0.30 limit, wrongly
  // approving a proposal whose TRUE combined share (0.588) is far over it.
  // With the final-overlay fix, leg0 is judged against the SAME complete
  // final projection leg1 sees, so the proposal now rejects ATOMICALLY at
  // commit -- arming and reserving NEITHER leg -- rather than silently
  // approving and only discovering the truth later, asymmetrically, at the
  // seam.
  RiskEngine engine(concentration_config(0.30));
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);
  seed_valid_quote(engine, kBg, 100);
  engine.on_fill(/*client_order_id=*/900, kBg, Side::kBuy, 300);
  engine.on_fill(/*client_order_id=*/901, kFar, Side::kBuy, 1'000);

  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kBuy, 100, 500, "strat", "n6", 0),
      make_request(kFar, Side::kSell, 100, 950, "strat", "n6", 1),
  };
  const auto& decision = engine.commit_proposal_decision("strat", "n6", legs, 0);
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kConcentration);
  ASSERT_EQ(decision.legs.size(), 2U);
  EXPECT_EQ(decision.legs[0].verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.legs[1].verdict, RiskVerdict::kReject);  // Never "leg0 reject, leg1 approve".

  // Nothing armed or reserved for either leg.
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
  const auto oms_decision = decide_registered_order(engine, "strat", "n6", /*leg_index=*/1, kFar,
                                                    Side::kSell, 950, /*client_order_id=*/1, 0);
  EXPECT_EQ(oms_decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(oms_decision.reason_code, ReasonCode::kUnexpectedOrder);  // No leg was ever armed.
}

TEST(ProposalAtomicSeamRevalidation,
     ALateStateChangeMakingOnlyOneLegUnsafeRejectsTheWholeProposalAtRelease) {
  // The generic version of the naked-leg hazard, unrelated to concentration:
  // a two-leg proposal commits safely; an out-of-band fill then pushes ONE
  // leg's OWN instrument over its position limit, while the other leg's
  // instrument is completely untouched and would individually still pass.
  // Under the release-epoch architecture the whole proposal is authorized or
  // not as a unit, so neither leg can execute -- and the order in which the
  // constituents would have been submitted is irrelevant, because nothing is
  // submittable at all.
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  // No limit configured for kFar: leg B's own control would pass in isolation.
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kBuy, 100, 50, "strat", "late-change", 0),  // leg A
      make_request(kFar, Side::kBuy, 100, 50, "strat", "late-change", 1),   // leg B
  };
  const auto& decision = engine.commit_proposal_decision("strat", "late-change", legs, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);  // Both safe at commit time.

  // Out-of-band: some OTHER, already-terminal order fills 60 more units of
  // kNear between commit and the release epoch. leg A's TRUE projected
  // exposure is now 60 (filled) + 50 (its own reservation) = 110 > 100.
  engine.on_fill(/*client_order_id=*/999, kNear, Side::kBuy, 60);

  const auto release =
      aegis::testing::stage_and_authorize(engine, "strat", "late-change",
                                          {aegis::testing::staged_leg(1, 0, kNear, Side::kBuy, 50),
                                           aegis::testing::staged_leg(2, 1, kFar, Side::kBuy, 50)});
  EXPECT_EQ(release.state, aegis::participant::risk::ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kMaxPositionLong);

  // Neither constituent can execute, in either submission order.
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kReject);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kReject);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
  EXPECT_EQ(engine.state().reservation_count(), 0U);
}

TEST(ProposalAtomicSeamRevalidation, SafeTwoLegProposalWithNoLateChangeStillApprovesBothLegs) {
  // Negative control for the test above: with NO out-of-band change, the
  // same shape of two-leg proposal must still authorize and let both legs
  // through -- proving release authorization does not spuriously reject when
  // nothing is actually wrong.
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kBuy, 100, 50, "strat", "no-late-change", 0),
      make_request(kFar, Side::kBuy, 100, 50, "strat", "no-late-change", 1),
  };
  const auto& decision = engine.commit_proposal_decision("strat", "no-late-change", legs, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);

  const auto release =
      aegis::testing::stage_and_authorize(engine, "strat", "no-late-change",
                                          {aegis::testing::staged_leg(1, 0, kNear, Side::kBuy, 50),
                                           aegis::testing::staged_leg(2, 1, kFar, Side::kBuy, 50)});
  ASSERT_EQ(release.state, aegis::participant::risk::ProposalReleaseState::kAuthorizedForRelease);

  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kApprove);
}

TEST(ProposalAtomicSeamRevalidation, ALookAlikeProposalCannotBorrowAnotherProposalsAuthorization) {
  // Attack D: two proposals with economically identical legs. Proposal A
  // becomes unsafe (out-of-band fill) and is correctly rejected at its
  // release epoch; proposal B must be judged on ITS OWN authorization, never
  // riding on A's -- the release state is keyed by the exact proposal_id,
  // resolved only through staged client_order_id identities.
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);

  engine.commit_proposal_decision(
      "strat-a", "look-a", {make_request(kNear, Side::kBuy, 100, 50, "strat-a", "look-a")}, 0);
  engine.commit_proposal_decision(
      "strat-b", "look-b", {make_request(kNear, Side::kBuy, 100, 50, "strat-b", "look-b")}, 0);

  // Push kNear's filled position up so that either reservation would breach.
  engine.on_fill(/*client_order_id=*/999, kNear, Side::kBuy, 60);

  const auto a_release = aegis::testing::stage_and_authorize(
      engine, "strat-a", "look-a", {aegis::testing::staged_leg(1, 0, kNear, Side::kBuy, 50)});
  EXPECT_EQ(a_release.state, aegis::participant::risk::ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(a_release.reason_code, ReasonCode::kMaxPositionLong);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kReject);

  // B's own reservation is untouched by A's rejection (a DIFFERENT
  // proposal_id) and B has its own, still-undecided release epoch.
  EXPECT_EQ(engine.state().leg_reservation_count(), 1U);
  EXPECT_EQ(engine.proposal_release_state("look-b"),
            aegis::participant::risk::ProposalReleaseState::kCommitted);
}

TEST(ProposalAtomicSeamRevalidation,
     PortfolioNotionalFinalOverlayApprovesAGenuinelySafeReducingProposal) {
  // The final-overlay fix is not only about preventing an unsafe proposal
  // from wrongly approving (N6) -- a stale prefix denominator can just as
  // easily cause a genuinely SAFE proposal to be wrongly rejected, which is
  // exactly as dishonest an outcome. Same "add + reduce" shape as N6, a
  // DIFFERENT cumulative control (portfolio notional, not concentration).
  //
  // Background: C (kFar) filled 1,000 @ 100 = 100,000.
  // Proposal: leg0 = A (kNear) buy 200 @ 100 = 20,000 (ADDS). leg1 = C sell
  //   950, reducing C to 50 units = 5,000 (REDUCES).
  // TRUE final combined notional = 20,000 + 5,000 = 25,000 <= the 30,000
  //   limit below -- genuinely safe.
  //
  // Under the OLD prefix-only preflight, leg0 (A) was evaluated BEFORE
  // leg1's reduction was staged, so it saw C's PRE-reduction notional:
  // 100,000 (C) + 20,000 (A) = 120,000 > 30,000 -- a spurious reject of a
  // proposal that was never actually unsafe.
  RiskLimitsConfig config = base_config();
  config.max_portfolio_notional_units = 30'000;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);
  engine.on_fill(/*client_order_id=*/900, kFar, Side::kBuy, 1'000);

  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kBuy, 100, 200, "strat", "notional-reduce", 0),
      make_request(kFar, Side::kSell, 100, 950, "strat", "notional-reduce", 1),
  };
  const auto& decision = engine.commit_proposal_decision("strat", "notional-reduce", legs, 0);
  EXPECT_EQ(decision.verdict, RiskVerdict::kApprove);
}

TEST(ProposalAtomicSeamRevalidation, FlatBookConcentrationBelowOneRejectsTheFirstPositionHonestly) {
  // Documented intended policy (ADR-0028; docs/LIMITATIONS.md), not a bug:
  // concentration is a SHARE of the whole portfolio. From a genuinely flat
  // book, the very first position any proposal takes IS, mathematically,
  // 100% of the (soon-to-exist) portfolio -- there is nothing else for it
  // to share space with. A configured max_concentration_share below 1.0
  // therefore rejects a lone first position honestly; this is the
  // mathematically correct reading of "share of portfolio", not an
  // off-by-one or an under-tested edge case. Forcing an exception here
  // (e.g. "concentration is disabled until N positions exist") would be
  // inventing policy the frozen requirement does not state -- so this test
  // exists to PIN the honest behavior, not to work around it.
  RiskEngine engine(concentration_config(0.50));
  seed_valid_quote(engine, kNear, 100);

  const auto& decision = engine.commit_proposal_decision(
      "strat", "flat-book", {make_request(kNear, Side::kBuy, 100, 10, "strat", "flat-book")}, 0);
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kConcentration);
}

}  // namespace
