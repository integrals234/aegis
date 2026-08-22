#include <gtest/gtest.h>

#include "cpp/participant/risk/risk_engine.hpp"
#include "tests/cpp/support/risk_seam_test_helpers.hpp"

/// M5 closure repair: the PROPOSAL RELEASE EPOCH (ADR-0027 "Correction 3").
///
/// An independent risk-safety review of the proposal-atomic seam repair found
/// two remaining defects, both rooted in the same mistake: the seam's
/// "revalidate the whole proposal once, then cache" epoch began at the moment
/// the FIRST leg reached `decide_order` -- i.e. at a point where that first
/// leg was already being released for execution.
///
///   BLOCKER A (over-cached safety): once the first leg's revalidation
///   passed, every later leg of the proposal consumed the cached verdict and
///   re-checked NOTHING -- not the kill switches, not connectivity, not
///   market staleness. A global kill switch tripped between leg 0's and
///   leg 1's arrival did not stop leg 1, contradicting AEGIS-135's
///   "prevents new orders".
///
///   BLOCKER B (identity mismatch strands a sibling): `decide_order`
///   rejected and released ONLY the mismatched leg, so a correct sibling
///   could still execute alone afterward -- a naked leg, contradicting the
///   whole-proposal atomicity the engine claimed.
///
/// The naive repair (cache cumulative controls, re-run hard safety per leg)
/// is explicitly NOT what this file tests: it would simply move the naked
/// leg (leg 0 released, state changes, leg 1 rejects). The actual fix moves
/// the epoch EARLIER instead: every constituent's identity and economics are
/// staged first, then ONE fresh whole-proposal authorization runs while
/// ZERO legs have been released, and only then may individual legs consume
/// that authorization.
namespace {

using aegis::common::Nanos;
using aegis::participant::risk::InstrumentInfo;
using aegis::participant::risk::OrderRequest;
using aegis::participant::risk::PositionLimit;
using aegis::participant::risk::ProposalReleaseState;
using aegis::participant::risk::ReasonCode;
using aegis::participant::risk::RiskEngine;
using aegis::participant::risk::RiskLimitsConfig;
using aegis::participant::risk::RiskVerdict;
using aegis::participant::risk::Side;
using aegis::participant::risk::StagedOrderIdentity;
using aegis::participant::risk::VolatilityReductionConfig;

constexpr std::uint32_t kNear = 4001;
constexpr std::uint32_t kFar = 4002;

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

/// The canonical two-leg spread every attack below starts from: near BUY 50,
/// far BUY 50, both at price 100.
std::vector<OrderRequest> two_leg_proposal(const std::string& strategy_id,
                                           const std::string& proposal_id) {
  return {make_request(kNear, Side::kBuy, 100, 50, strategy_id, proposal_id, 0),
          make_request(kFar, Side::kBuy, 100, 50, strategy_id, proposal_id, 1)};
}

/// Stages both constituents of `two_leg_proposal` with the client_order_ids
/// the caller will actually submit under, exactly as the composition root
/// does before calling `authorize_proposal_release`.
std::vector<StagedOrderIdentity> stage_two_legs(std::uint64_t leg0_client_order_id,
                                                std::uint64_t leg1_client_order_id) {
  return {StagedOrderIdentity{.client_order_id = leg0_client_order_id,
                              .leg_index = 0,
                              .instrument_id = kNear,
                              .side = Side::kBuy,
                              .quantity_units = 50},
          StagedOrderIdentity{.client_order_id = leg1_client_order_id,
                              .leg_index = 1,
                              .instrument_id = kFar,
                              .side = Side::kBuy,
                              .quantity_units = 50}};
}

// =================================================================
// BLOCKER A -- over-cached safety
// =================================================================

TEST(ProposalReleaseEpoch, GlobalKillSwitchBeforeReleaseRejectsTheWholeProposal) {
  // ATTACK 1. The kill switch trips AFTER commit but BEFORE the release
  // authorization -- i.e. while zero legs have been released. The whole
  // proposal must be rejected and every reservation returned.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat", "kill-before",
                                          two_leg_proposal("strat", "kill-before"), 0)
                .verdict,
            RiskVerdict::kApprove);
  ASSERT_EQ(engine.state().leg_reservation_count(), 2U);

  engine.stage_proposal_release("strat", "kill-before", stage_two_legs(1, 2));
  ASSERT_TRUE(engine.trip_global());

  const auto release = engine.authorize_proposal_release("strat", "kill-before", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kKillSwitchGlobal);

  // Zero constituents executable, zero reservations held.
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kReject);
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kReject);
  EXPECT_EQ(engine.state().reservation_count(), 0U);
}

TEST(ProposalReleaseEpoch, ExchangeDisconnectBeforeReleaseRejectsTheWholeProposal) {
  // ATTACK 2.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat", "disc-before",
                                          two_leg_proposal("strat", "disc-before"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "disc-before", stage_two_legs(1, 2));
  engine.on_exchange_disconnected();

  const auto release = engine.authorize_proposal_release("strat", "disc-before", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kExchangeDisconnected);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kReject);
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kReject);
}

TEST(ProposalReleaseEpoch, StaleMarketBeforeReleaseRejectsTheWholeProposal) {
  // ATTACK 3. max_quote_age is 100ns; the quote is seeded at t=0 and the
  // release authorization runs at t=500, so the reference is stale by the
  // engine's own injected-clock rule (never wall clock).
  RiskLimitsConfig config = base_config();
  config.max_quote_age = aegis::common::Duration{100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, /*observed_at_nanos=*/0);
  seed_valid_quote(engine, kFar, 100, /*observed_at_nanos=*/0);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat", "stale-before",
                                          two_leg_proposal("strat", "stale-before"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "stale-before", stage_two_legs(1, 2));

  const auto release =
      engine.authorize_proposal_release("strat", "stale-before", /*now_nanos=*/500);
  EXPECT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kStaleMarketData);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
}

TEST(ProposalReleaseEpoch, ExposureChangeBeforeReleaseRejectsTheWholeProposal) {
  // The cumulative half of the same epoch: an out-of-band fill lands between
  // commit and release authorization, making ONE leg's own position limit
  // breach. The OTHER leg's instrument is untouched and unlimited -- yet the
  // whole proposal must reject.
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat", "expo-before",
                                          two_leg_proposal("strat", "expo-before"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "expo-before", stage_two_legs(1, 2));
  engine.on_fill(/*client_order_id=*/999, kNear, Side::kBuy, 60);  // 60 + 50 reserved > 100.

  const auto release = engine.authorize_proposal_release("strat", "expo-before", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kMaxPositionLong);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
  // The untouched sibling cannot execute alone.
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kReject);
}

// =================================================================
// BLOCKER B -- identity mismatch must condemn the whole proposal
// =================================================================

TEST(ProposalReleaseEpoch, IdentityMismatchAtStagingRejectsTheWholeProposalAndStrandsNoSibling) {
  // ATTACK 4. Leg 0 is staged with a quantity that disagrees with what the
  // proposal actually committed (51, not 50). Because ALL constituents are
  // validated at the release epoch -- before ANY leg is released -- the
  // whole proposal is rejected, so the CORRECT sibling can never execute
  // alone. Under the previous design, leg 0 rejected kIdentityMismatch at
  // its own decide_order and leg 1 then approved by itself: a naked leg.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(
      engine.commit_proposal_decision("strat", "mismatch", two_leg_proposal("strat", "mismatch"), 0)
          .verdict,
      RiskVerdict::kApprove);

  std::vector<StagedOrderIdentity> staged = stage_two_legs(1, 2);
  staged[0].quantity_units = 51;  // Disagrees with the committed leg.
  engine.stage_proposal_release("strat", "mismatch", staged);

  const auto release = engine.authorize_proposal_release("strat", "mismatch", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kIdentityMismatch);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);

  // The correctly-staged sibling is dead too -- the whole point.
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kReject);
  EXPECT_EQ(engine.state().reservation_count(), 0U);
}

// =================================================================
// Incomplete staging, valid release, and cross-proposal isolation
// =================================================================

TEST(ProposalReleaseEpoch, AnIncompletelyStagedProposalIsNeverAuthorized) {
  // ATTACK 5. Only leg 0 is staged. The proposal must NOT become executable,
  // and no leg may fake an approve.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(
      engine.commit_proposal_decision("strat", "partial", two_leg_proposal("strat", "partial"), 0)
          .verdict,
      RiskVerdict::kApprove);

  const std::vector<StagedOrderIdentity> only_leg_zero = {stage_two_legs(1, 2).front()};
  engine.stage_proposal_release("strat", "partial", only_leg_zero);

  const auto release = engine.authorize_proposal_release("strat", "partial", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kIncompleteProposalStaging);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kReject);
}

TEST(ProposalReleaseEpoch, AnUnauthorizedProposalsLegCannotReachExecution) {
  // The structural default: staging alone is not authorization. A caller
  // that skips authorize_proposal_release entirely gets nothing executable.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(
      engine.commit_proposal_decision("strat", "unauth", two_leg_proposal("strat", "unauth"), 0)
          .verdict,
      RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "unauth", stage_two_legs(1, 2));

  const auto decision = engine.decide_order(kNear, Side::kBuy, 50, 1, 0);
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kProposalNotAuthorized);
}

TEST(ProposalReleaseEpoch, AFullyValidProposalAuthorizesOnceAndBothLegsConsumeIt) {
  // ATTACK 6. Everything valid: one authorization, both exact legs consume
  // it, each exactly once.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine.commit_proposal_decision("strat", "valid", two_leg_proposal("strat", "valid"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "valid", stage_two_legs(1, 2));

  const auto release = engine.authorize_proposal_release("strat", "valid", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kAuthorizedForRelease);

  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kApprove);
  EXPECT_EQ(engine.state().reservation_count(), 2U);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);

  // Each authorization is consumed at most once per leg.
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kReject);
}

TEST(ProposalReleaseEpoch, ALookAlikeProposalCannotConsumeAnothersAuthorization) {
  // ATTACK 7. Two proposals, economically identical legs. Only "auth-a" is
  // authorized; "auth-b"'s constituents must not ride on it.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(
      engine.commit_proposal_decision("strat-a", "auth-a", two_leg_proposal("strat-a", "auth-a"), 0)
          .verdict,
      RiskVerdict::kApprove);
  ASSERT_EQ(
      engine.commit_proposal_decision("strat-b", "auth-b", two_leg_proposal("strat-b", "auth-b"), 0)
          .verdict,
      RiskVerdict::kApprove);

  engine.stage_proposal_release("strat-a", "auth-a", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat-a", "auth-a", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);

  // B is committed and reserved but never staged/authorized: its legs are
  // not executable, and A's authorization does not cover them.
  engine.stage_proposal_release("strat-b", "auth-b", stage_two_legs(3, 4));
  const auto b_leg = engine.decide_order(kNear, Side::kBuy, 50, 3, 0);
  EXPECT_EQ(b_leg.verdict, RiskVerdict::kReject);
  EXPECT_EQ(b_leg.reason_code, ReasonCode::kProposalNotAuthorized);

  // A's own legs still work.
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
}

// =================================================================
// Post-authorization timing semantics (the epoch's deliberate boundary)
// =================================================================

TEST(ProposalReleaseEpoch, AKillSwitchAfterAuthorizationDoesNotSplitTheProposalsVerdict) {
  // The semantics this architecture deliberately chooses, pinned so a future
  // refactor cannot silently restore the per-leg conflict:
  //
  // Once the whole proposal is AUTHORIZED FOR RELEASE, it is ONE already-made
  // risk decision. A kill switch tripping afterwards must NOT retroactively
  // reject a remaining constituent, because doing so would produce exactly
  // the mixed verdict (leg 0 released, leg 1 rejected -> naked leg) this
  // repair exists to prevent. The kill switch instead blocks all
  // SUBSEQUENT proposals, and live orders are handled by the existing
  // emergency-cancel path.
  //
  // This is RISK-DECISION atomicity, not exchange-execution atomicity: the
  // transport can still fail one leg after authorization, and AEGIS does not
  // claim otherwise (docs/LIMITATIONS.md).
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(
      engine
          .commit_proposal_decision("strat", "post-auth", two_leg_proposal("strat", "post-auth"), 0)
          .verdict,
      RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "post-auth", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat", "post-auth", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);

  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
  ASSERT_TRUE(engine.trip_global());

  // Leg 1 still consumes the authorization its proposal already received --
  // no contradictory mixed verdict.
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kApprove);

  // But a NEW proposal is blocked outright, which is where AEGIS-135's
  // "prevents new orders" actually bites.
  const auto& blocked = engine.commit_proposal_decision("strat", "after-kill",
                                                        two_leg_proposal("strat", "after-kill"), 0);
  EXPECT_EQ(blocked.verdict, RiskVerdict::kReject);
  EXPECT_EQ(blocked.reason_code, ReasonCode::kKillSwitchGlobal);
}

// =================================================================
// R3: abort_proposal_release reclaims an authorized proposal's still-
// unconsumed reservations without rolling back anything already consumed.
// =================================================================

TEST(ProposalAbort, AuthorizedProposalWithNothingConsumedReleasesBothLegsOnAbort) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat", "abort-zero",
                                          two_leg_proposal("strat", "abort-zero"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "abort-zero", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat", "abort-zero", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
  ASSERT_EQ(engine.state().leg_reservation_count(), 2U);

  const auto abort = engine.abort_proposal_release("strat", "abort-zero", "never submitted", 0);
  EXPECT_EQ(abort.state, ProposalReleaseState::kAborted);
  EXPECT_EQ(abort.reason_code, ReasonCode::kProposalAborted);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);  // Both legs' reservations reclaimed.

  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kReject);
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kReject);
}

TEST(ProposalAbort, ConsumedLegSurvivesAbortAndOnlyTheRemainingLegIsReleased) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(
      engine
          .commit_proposal_decision("strat", "abort-one", two_leg_proposal("strat", "abort-one"), 0)
          .verdict,
      RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "abort-one", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat", "abort-one", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);

  // Leg 0 actually consumes its authorization (an order-keyed reservation,
  // no longer leg-keyed).
  ASSERT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
  ASSERT_EQ(engine.state().reservation_count(), 1U);
  ASSERT_EQ(engine.state().leg_reservation_count(), 1U);  // Leg 1 still unconsumed.

  const auto abort = engine.abort_proposal_release("strat", "abort-one", "far leg abandoned", 0);
  EXPECT_EQ(abort.state, ProposalReleaseState::kAborted);

  // Leg 0's already-live reservation is untouched; only leg 1's was released.
  EXPECT_EQ(engine.state().reservation_count(), 1U);
  EXPECT_EQ(engine.state().leg_reservation_count(), 0U);
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kReject);
}

TEST(ProposalAbort, DoubleAbortIsIdempotentAndRecordsNoSecondAuditEntry) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat", "abort-twice",
                                          two_leg_proposal("strat", "abort-twice"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "abort-twice", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat", "abort-twice", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);

  const auto first = engine.abort_proposal_release("strat", "abort-twice", "first", 0);
  const auto second = engine.abort_proposal_release("strat", "abort-twice", "second", 0);
  EXPECT_EQ(first.state, ProposalReleaseState::kAborted);
  EXPECT_EQ(second.state, ProposalReleaseState::kAborted);
  // The SECOND call's reason text is discarded, not recorded -- abort is
  // terminal after the FIRST call, exactly like authorize_proposal_release.
  EXPECT_EQ(second.reason, "first");
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("abort-twice"), 2U);
}

TEST(ProposalAbort, AbortingAProposalAlreadyRejectedAtReleaseIsANoOp) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat", "abort-rejected",
                                          two_leg_proposal("strat", "abort-rejected"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "abort-rejected", stage_two_legs(1, 2));
  ASSERT_TRUE(engine.trip_global());
  const auto release = engine.authorize_proposal_release("strat", "abort-rejected", 0);
  ASSERT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);

  const auto abort = engine.abort_proposal_release("strat", "abort-rejected", "irrelevant", 0);
  EXPECT_EQ(abort.state, ProposalReleaseState::kRejectedAtRelease);  // Unchanged.
  EXPECT_EQ(abort.reason_code, release.reason_code);
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("abort-rejected"), 1U);
}

TEST(ProposalAbort, AbortingAProposalAlreadyCompletedIsANoOp) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat", "abort-completed",
                                          two_leg_proposal("strat", "abort-completed"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "abort-completed", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat", "abort-completed", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
  ASSERT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
  ASSERT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kApprove);
  ASSERT_EQ(engine.proposal_release_state("abort-completed"), ProposalReleaseState::kCompleted);

  const auto abort = engine.abort_proposal_release("strat", "abort-completed", "too late", 0);
  EXPECT_EQ(abort.state, ProposalReleaseState::kCompleted);  // Unchanged; nothing to reclaim.
  EXPECT_EQ(engine.state().reservation_count(), 2U);  // Both live orders' reservations intact.
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("abort-completed"), 1U);
}

// =================================================================
// R5: canonical proposal/strategy attribution is immutable.
// =================================================================

TEST(ProposalAttribution, StagingWithADisagreeingStrategyIdCannotOverwriteCanonicalAttribution) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-1",
                                          two_leg_proposal("strat-a", "attrib-1"), 0)
                .verdict,
            RiskVerdict::kApprove);

  // An attacker (or a colliding proposal_id from a different strategy) tries
  // to stage the SAME proposal_id under a different strategy_id.
  engine.stage_proposal_release("strat-b", "attrib-1", stage_two_legs(1, 2));

  const auto release = engine.authorize_proposal_release("strat-a", "attrib-1", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kIdentityMismatch);

  // The audit trail attributes the rejection to the CANONICAL committing
  // strategy, never to the attacker's identity -- and this remains the
  // proposal's ONE terminal release decision.
  ASSERT_FALSE(engine.audit_log().proposal_release_decisions().empty());
  EXPECT_EQ(engine.audit_log().proposal_release_decisions().back().strategy_id, "strat-a");
  EXPECT_EQ(engine.audit_log().proposal_decisions().back().strategy_id, "strat-a");
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("attrib-1"), 1U);
  EXPECT_EQ(engine.audit_log().proposal_decision_count("attrib-1"), 1U);
}

TEST(ProposalAttribution, ALegitimateRestageAfterAPoisoningAttemptIsStillRejectedFailClosed) {
  // Once a mismatched strategy_id has touched a proposal_id, that proposal
  // can never be trusted again -- even a subsequent, correctly-attributed
  // stage call from the legitimate strategy does not un-poison it.
  // Deliberately fail-closed: RiskEngine cannot tell "the attacker's call was
  // bogus" from "the LEGITIMATE call was bogus" after the fact, so it never
  // lets EITHER execute.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-2",
                                          two_leg_proposal("strat-a", "attrib-2"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-b", "attrib-2", stage_two_legs(9, 10));
  engine.stage_proposal_release("strat-a", "attrib-2", stage_two_legs(1, 2));  // Legitimate, after.

  const auto release = engine.authorize_proposal_release("strat-a", "attrib-2", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kIdentityMismatch);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kReject);
}

TEST(ProposalAttribution, MismatchedStageCallBindsNoneOfItsOwnIdentities) {
  // The attacker's own client_order_ids must never resolve to anything --
  // not even to a rejected identity -- so a caller cannot fish for
  // information about a proposal it does not own.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-3",
                                          two_leg_proposal("strat-a", "attrib-3"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-b", "attrib-3", stage_two_legs(9, 10));

  const auto attacker_order = engine.decide_order(kNear, Side::kBuy, 50, 9, 0);
  EXPECT_EQ(attacker_order.verdict, RiskVerdict::kReject);
  EXPECT_EQ(attacker_order.reason_code, ReasonCode::kUnexpectedOrder);
}

// =================================================================
// N4: authorize_proposal_release must verify the CALLER's own strategy_id
// against the proposal's canonical committed strategy_id -- staging was
// already fail-closed on a mismatch (R5); authorizing was not.
// =================================================================

TEST(ProposalAttribution,
     AuthorizeWithAWrongStrategyIdIsAnUnauthorizedQueryThatLeavesTheVictimCompletelyUnchanged) {
  // THE MOST IMPORTANT N4 REGRESSION (M5 closure repair, N4 corrected): a
  // wrong-strategy authorize call is an UNAUTHORIZED QUERY, not a risk
  // rejection of the proposal it names. It must not mutate the victim's
  // proposal in any way, and the victim must be able to authorize normally
  // afterward -- exactly as if the attacker's call had never happened.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-4",
                                          two_leg_proposal("strat-a", "attrib-4"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "attrib-4", stage_two_legs(1, 2));

  // Capture every observable piece of victim state before the attack.
  const auto state_before = engine.proposal_release_state("attrib-4");
  const auto reserved_near_before = engine.state().reserved_units(kNear);
  const auto reserved_far_before = engine.state().reserved_units(kFar);
  const auto leg_reservation_count_before = engine.state().leg_reservation_count();
  const auto release_decision_count_before =
      engine.audit_log().proposal_release_decision_count("attrib-4");

  ASSERT_EQ(state_before, ProposalReleaseState::kStaging);
  ASSERT_EQ(reserved_near_before, 50);
  ASSERT_EQ(reserved_far_before, 50);
  ASSERT_EQ(leg_reservation_count_before, 2U);
  ASSERT_EQ(release_decision_count_before, 0U);

  // The attacker never staged anything under its own name -- it simply
  // calls authorize with a strategy_id that disagrees with the canonical
  // committer.
  const auto attacker_release = engine.authorize_proposal_release("strat-attacker", "attrib-4", 0);
  EXPECT_NE(attacker_release.state, ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(attacker_release.reason_code, ReasonCode::kIdentityMismatch);

  // Every piece of victim state is BIT-FOR-BIT unchanged -- no mutation, no
  // reservation release, no pending-leg erasure, no new audit event.
  EXPECT_EQ(engine.proposal_release_state("attrib-4"), state_before);
  EXPECT_EQ(engine.state().reserved_units(kNear), reserved_near_before);
  EXPECT_EQ(engine.state().reserved_units(kFar), reserved_far_before);
  EXPECT_EQ(engine.state().leg_reservation_count(), leg_reservation_count_before);
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("attrib-4"),
            release_decision_count_before);

  // The canonical owner can now authorize completely normally -- the
  // attacker's call left no trace.
  const auto real_release = engine.authorize_proposal_release("strat-a", "attrib-4", 0);
  EXPECT_EQ(real_release.state, ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(engine.audit_log().proposal_release_decisions().back().strategy_id, "strat-a");
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kApprove);
}

TEST(ProposalAttribution, RiskBudgetTheftAttackRemainsBlockedAfterAWrongStrategyAuthorizeAttempt) {
  // The reviewer's specific observation: the OLD bug freed the victim's
  // reserved risk budget, letting the attacker's own later proposal fit
  // into capacity that should still have been claimed by the victim.
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 50, .max_short_units = 50};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  // Victim commits and stages a proposal that consumes the ENTIRE kNear
  // position capacity (50 of 50).
  ASSERT_EQ(
      engine
          .commit_proposal_decision("victim", "budget-1", two_leg_proposal("victim", "budget-1"), 0)
          .verdict,
      RiskVerdict::kApprove);
  engine.stage_proposal_release("victim", "budget-1", stage_two_legs(1, 2));
  ASSERT_EQ(engine.state().reserved_units(kNear), 50);

  // Attacker attempts to authorize the victim's proposal under its own
  // identity -- denied, and (per the test above) the victim's reservation
  // must survive this attempt untouched.
  const auto attacker_release = engine.authorize_proposal_release("attacker", "budget-1", 0);
  EXPECT_NE(attacker_release.state, ProposalReleaseState::kAuthorizedForRelease);
  ASSERT_EQ(engine.state().reserved_units(kNear), 50);  // Still fully reserved.

  // The attacker's OWN proposal on the same instrument would only fit if
  // the victim's 50-unit reservation had actually been released -- it must
  // still be rejected by the position cap.
  const auto attacker_own_proposal = engine.commit_proposal_decision(
      "attacker", "budget-2", {make_request(kNear, Side::kBuy, 100, 50, "attacker", "budget-2", 0)},
      0);
  EXPECT_EQ(attacker_own_proposal.verdict, RiskVerdict::kReject);
  EXPECT_EQ(attacker_own_proposal.reason_code, ReasonCode::kMaxPositionLong);

  // The victim can still legitimately authorize and execute its own
  // proposal afterward -- nothing about its lifecycle was disturbed.
  ASSERT_EQ(engine.authorize_proposal_release("victim", "budget-1", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kApprove);
}

TEST(ProposalAttribution, WrongStrategyQueryOfAnAuthorizedProposalNeverDisclosesTheRealDecision) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "term-auth",
                                          two_leg_proposal("strat-a", "term-auth"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "term-auth", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat-a", "term-auth", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
  const auto count_before = engine.audit_log().proposal_release_decision_count("term-auth");

  // The proposal is now genuinely kAuthorizedForRelease. A wrong-strategy
  // query must NOT receive that real state -- only the generic denial.
  const auto attacker_query = engine.authorize_proposal_release("attacker", "term-auth", 0);
  EXPECT_NE(attacker_query.state, ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(attacker_query.reason_code, ReasonCode::kIdentityMismatch);
  EXPECT_EQ(engine.proposal_release_state("term-auth"),
            ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("term-auth"), count_before);

  // The canonical owner's own legs are still fully executable.
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
}

TEST(ProposalAttribution, WrongStrategyQueryOfARejectedProposalNeverDisclosesTheRealDecision) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "term-reject",
                                          two_leg_proposal("strat-a", "term-reject"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "term-reject", stage_two_legs(1, 2));
  ASSERT_TRUE(engine.trip_global());
  const auto real_release = engine.authorize_proposal_release("strat-a", "term-reject", 0);
  ASSERT_EQ(real_release.state, ProposalReleaseState::kRejectedAtRelease);
  ASSERT_EQ(real_release.reason_code, ReasonCode::kKillSwitchGlobal);
  const auto count_before = engine.audit_log().proposal_release_decision_count("term-reject");

  const auto attacker_query = engine.authorize_proposal_release("attacker", "term-reject", 0);
  EXPECT_EQ(attacker_query.reason_code, ReasonCode::kIdentityMismatch);
  // The attacker never sees the REAL rejection reason (kKillSwitchGlobal).
  EXPECT_NE(attacker_query.reason_code, ReasonCode::kKillSwitchGlobal);
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("term-reject"), count_before);
}

TEST(ProposalAttribution, WrongStrategyQueryOfAnAbortedProposalNeverDisclosesTheRealDecision) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "term-abort",
                                          two_leg_proposal("strat-a", "term-abort"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "term-abort", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat-a", "term-abort", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
  ASSERT_EQ(engine.abort_proposal_release("strat-a", "term-abort", "victim's own choice", 0).state,
            ProposalReleaseState::kAborted);
  const auto count_before = engine.audit_log().proposal_release_decision_count("term-abort");

  const auto attacker_query = engine.authorize_proposal_release("attacker", "term-abort", 0);
  EXPECT_EQ(attacker_query.reason_code, ReasonCode::kIdentityMismatch);
  EXPECT_NE(attacker_query.reason_code, ReasonCode::kProposalAborted);
  EXPECT_NE(attacker_query.reason, "victim's own choice");
  EXPECT_EQ(engine.proposal_release_state("term-abort"), ProposalReleaseState::kAborted);
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("term-abort"), count_before);
}

TEST(ProposalAttribution, WrongStrategyQueryOfACompletedProposalNeverDisclosesTheRealDecision) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "term-complete",
                                          two_leg_proposal("strat-a", "term-complete"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "term-complete", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat-a", "term-complete", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
  ASSERT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
  ASSERT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kApprove);
  ASSERT_EQ(engine.proposal_release_state("term-complete"), ProposalReleaseState::kCompleted);

  // A completed proposal is no longer executable regardless, but a
  // wrong-strategy caller must still receive only the generic denial, not
  // an implicit confirmation that the proposal reached kCompleted.
  const auto attacker_query = engine.authorize_proposal_release("attacker", "term-complete", 0);
  EXPECT_NE(attacker_query.state, ProposalReleaseState::kCompleted);
  EXPECT_EQ(attacker_query.reason_code, ReasonCode::kIdentityMismatch);
  EXPECT_EQ(engine.proposal_release_state("term-complete"), ProposalReleaseState::kCompleted);
}

TEST(ProposalAttribution, AuthorizeWithTheCorrectStrategyIdStillWorksNormally) {
  // Control for the N4 attack test above: the legitimate committer's own
  // authorize call must be completely unaffected.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-5",
                                          two_leg_proposal("strat-a", "attrib-5"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "attrib-5", stage_two_legs(1, 2));

  EXPECT_EQ(engine.authorize_proposal_release("strat-a", "attrib-5", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
}

// =================================================================
// N5: canonical strategy attribution ORIGINATES at commit_proposal_decision
// only -- a pre-commit stage/authorize/abort call on an unknown
// proposal_id must never be able to establish ownership, so a later
// legitimate commit needs no "overwrite" to correct it.
// =================================================================

TEST(ProposalAttribution, APreCommitStageCallCannotEstablishCanonicalOwnership) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  // Before proposal "attrib-6" is ever committed, an attacker (or a
  // colliding proposal_id from a different strategy) tries to stage it
  // under its own name.
  engine.stage_proposal_release("strat-attacker", "attrib-6", stage_two_legs(9, 10));

  // The attacker's own staged identities resolve to nothing.
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 9, 0).reason_code,
            ReasonCode::kUnexpectedOrder);

  // The legitimate strategy now commits the SAME proposal_id. No overwrite
  // was necessary -- the attacker never actually owned anything.
  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-6",
                                          two_leg_proposal("strat-a", "attrib-6"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "attrib-6", stage_two_legs(1, 2));
  const auto release = engine.authorize_proposal_release("strat-a", "attrib-6", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
}

TEST(ProposalAttribution, APreCommitAuthorizeCallCreatesNoPersistentState) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  const auto premature = engine.authorize_proposal_release("strat-a", "attrib-7", 0);
  EXPECT_NE(premature.state, ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(engine.proposal_release_state("attrib-7"), ProposalReleaseState::kCommitted);

  // A genuine commit afterward is completely unaffected by the premature call.
  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-7",
                                          two_leg_proposal("strat-a", "attrib-7"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "attrib-7", stage_two_legs(1, 2));
  EXPECT_EQ(engine.authorize_proposal_release("strat-a", "attrib-7", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
}

// =================================================================
// N6: abort_proposal_release must require the caller's own strategy_id and
// must never create persistent state for an unknown/uncommitted proposal.
// =================================================================

TEST(ProposalAttribution, AbortOfAnUnknownProposalCreatesNoStateThatPoisonsALaterCommit) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  const auto premature_abort = engine.abort_proposal_release("strat-a", "attrib-8", "too early", 0);
  EXPECT_NE(premature_abort.state, ProposalReleaseState::kAborted);
  EXPECT_EQ(engine.proposal_release_state("attrib-8"), ProposalReleaseState::kCommitted);

  // The proposal_id is still fully usable afterward -- the pre-emptive
  // abort left no trace that could block a later legitimate commit.
  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-8",
                                          two_leg_proposal("strat-a", "attrib-8"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "attrib-8", stage_two_legs(1, 2));
  EXPECT_EQ(engine.authorize_proposal_release("strat-a", "attrib-8", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
}

TEST(ProposalAttribution, AbortByAWrongStrategyLeavesTheCanonicalProposalUnaffected) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-9",
                                          two_leg_proposal("strat-a", "attrib-9"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "attrib-9", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat-a", "attrib-9", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);

  // B (who never committed or owns anything here) tries to abort A's
  // authorized proposal.
  const auto wrong_strategy_abort =
      engine.abort_proposal_release("strat-b", "attrib-9", "not mine to abort", 0);
  EXPECT_NE(wrong_strategy_abort.state, ProposalReleaseState::kAborted);

  // A's proposal is completely unaffected: still authorized, still executable.
  EXPECT_EQ(engine.proposal_release_state("attrib-9"), ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kApprove);
  EXPECT_EQ(engine.decide_order(kFar, Side::kBuy, 50, 2, 0).verdict, RiskVerdict::kApprove);
}

TEST(ProposalAttribution, AbortByTheCanonicalStrategyStillWorksNormally) {
  // Control for the N6 attack tests above.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat-a", "attrib-10",
                                          two_leg_proposal("strat-a", "attrib-10"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat-a", "attrib-10", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat-a", "attrib-10", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);

  const auto abort = engine.abort_proposal_release("strat-a", "attrib-10", "genuinely mine", 0);
  EXPECT_EQ(abort.state, ProposalReleaseState::kAborted);
  EXPECT_EQ(engine.decide_order(kNear, Side::kBuy, 50, 1, 0).verdict, RiskVerdict::kReject);
}

// =================================================================
// N1: authorize_proposal_release AFTER a deliberate abort must be a pure
// no-op -- no state change, no new release-lifecycle audit event, and
// decide_order must report the ABORT reason, never a generic/unexpected
// rejection.
// =================================================================

TEST(ProposalAbort, AuthorizeAfterAbortRecordsNoNewEventAndDecideOrderReportsAborted) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(engine
                .commit_proposal_decision("strat", "abort-then-reauth",
                                          two_leg_proposal("strat", "abort-then-reauth"), 0)
                .verdict,
            RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "abort-then-reauth", stage_two_legs(1, 2));
  ASSERT_EQ(engine.authorize_proposal_release("strat", "abort-then-reauth", 0).state,
            ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("abort-then-reauth"), 1U);

  const auto abort =
      engine.abort_proposal_release("strat", "abort-then-reauth", "changed my mind", 0);
  ASSERT_EQ(abort.state, ProposalReleaseState::kAborted);
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("abort-then-reauth"), 2U);

  // N1: authorize AFTER abort must be a pure no-op -- no state change, no
  // new audit event, and it must NOT resurrect the proposal as if the
  // abort never happened.
  const auto reauthorize = engine.authorize_proposal_release("strat", "abort-then-reauth", 10);
  EXPECT_EQ(reauthorize.state, ProposalReleaseState::kAborted);
  EXPECT_EQ(reauthorize.reason_code, ReasonCode::kProposalAborted);
  EXPECT_EQ(reauthorize.reason, "changed my mind");
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count("abort-then-reauth"),
            2U);  // Unchanged.
  EXPECT_EQ(engine.proposal_release_state("abort-then-reauth"), ProposalReleaseState::kAborted);

  // decide_order after abort reports the ABORT reason, not a generic/
  // unexpected-order rejection -- attribution of WHY is preserved.
  const auto near_order = engine.decide_order(kNear, Side::kBuy, 50, 1, 0);
  EXPECT_EQ(near_order.verdict, RiskVerdict::kReject);
  EXPECT_EQ(near_order.reason_code, ReasonCode::kProposalAborted);
  EXPECT_NE(near_order.reason_code, ReasonCode::kUnexpectedOrder);

  // AEGIS-137's OWN invariant -- exactly one ProposalRiskDecision -- is
  // untouched by any of the release-lifecycle churn above.
  EXPECT_EQ(engine.audit_log().proposal_decision_count("abort-then-reauth"), 1U);
}

// =================================================================
// R6: the volatility HARD-REJECT safety gate is re-checked fresh at
// release, unlike the immutable resize/sizing decision (which is not).
// =================================================================

TEST(ProposalReleaseEpoch, VolatilitySpikeBeforeReleaseRejectsTheWholeProposal) {
  RiskLimitsConfig config = base_config();
  config.volatility = VolatilityReductionConfig{
      .window = 5, .target_volatility = 0.01, .hard_reject_multiple = 3.0};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  // Calm at commit: no return series fed yet, so realized_volatility() is
  // 0.0 and neither resize nor hard-reject fires.
  ASSERT_EQ(
      engine
          .commit_proposal_decision("strat", "vol-spike", two_leg_proposal("strat", "vol-spike"), 0)
          .verdict,
      RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "vol-spike", stage_two_legs(1, 2));

  // A large, sustained move between commit and release pushes realized
  // volatility on kNear's own reference series past hard_reject_multiple *
  // target_volatility.
  std::int64_t price = 100;
  for (int i = 0; i < 6; ++i) {
    price = (i % 2 == 0) ? price * 2 : price / 2;  // +-100%/-50% swings.
    engine.on_market_data(kNear, price, i + 1, true);
  }

  const auto release = engine.authorize_proposal_release("strat", "vol-spike", 10);
  EXPECT_EQ(release.state, ProposalReleaseState::kRejectedAtRelease);
  EXPECT_EQ(release.reason_code, ReasonCode::kVolatilityReduction);
  EXPECT_EQ(engine.state().leg_reservation_count(),
            0U);  // Whole proposal released, not just kNear.
}

TEST(ProposalReleaseEpoch, CalmVolatilityThroughReleaseStillAuthorizesNormally) {
  RiskLimitsConfig config = base_config();
  config.volatility = VolatilityReductionConfig{
      .window = 5, .target_volatility = 0.01, .hard_reject_multiple = 3.0};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100);
  seed_valid_quote(engine, kFar, 100);

  ASSERT_EQ(
      engine.commit_proposal_decision("strat", "vol-calm", two_leg_proposal("strat", "vol-calm"), 0)
          .verdict,
      RiskVerdict::kApprove);
  engine.stage_proposal_release("strat", "vol-calm", stage_two_legs(1, 2));

  const auto release = engine.authorize_proposal_release("strat", "vol-calm", 0);
  EXPECT_EQ(release.state, ProposalReleaseState::kAuthorizedForRelease);
}

}  // namespace
