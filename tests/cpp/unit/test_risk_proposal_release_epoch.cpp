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

  const auto abort = engine.abort_proposal_release("abort-zero", "never submitted", 0);
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

  const auto abort = engine.abort_proposal_release("abort-one", "far leg abandoned", 0);
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

  const auto first = engine.abort_proposal_release("abort-twice", "first", 0);
  const auto second = engine.abort_proposal_release("abort-twice", "second", 0);
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

  const auto abort = engine.abort_proposal_release("abort-rejected", "irrelevant", 0);
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

  const auto abort = engine.abort_proposal_release("abort-completed", "too late", 0);
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
