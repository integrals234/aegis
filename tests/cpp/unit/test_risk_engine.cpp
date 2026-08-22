#include <gtest/gtest.h>

#include "cpp/participant/risk/risk_engine.hpp"
#include "tests/cpp/support/risk_seam_test_helpers.hpp"

/// AEGIS-121..137: the M5 risk engine's pre-trade controls and audit
/// invariant, exercised directly against `RiskEngine`'s public API -- the
/// same `evaluate`/`evaluate_proposal`/`commit_proposal_decision`/
/// `decide_order` methods `RiskEngineGate` calls at the OMS seam
/// (`tests/cpp/unit/test_calendar_spread_risk_exchange_integration.cpp`
/// proves the same engine through that full seam against a real M1
/// `ExchangeNode`).
namespace {

using aegis::common::Duration;
using aegis::common::Nanos;
using aegis::participant::risk::InstrumentInfo;
using aegis::participant::risk::LegDecision;
using aegis::participant::risk::OrderQuantityLimit;
using aegis::participant::risk::OrderRequest;
using aegis::participant::risk::PositionLimit;
using aegis::participant::risk::ReasonCode;
using aegis::participant::risk::RiskEngine;
using aegis::participant::risk::RiskLimitsConfig;
using aegis::participant::risk::RiskVerdict;
using aegis::participant::risk::Side;
using aegis::participant::risk::VolatilityReductionConfig;
using aegis::testing::decide_registered_order;

constexpr std::uint32_t kNear = 2001;
constexpr std::uint32_t kFar = 2002;

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
                      Nanos observed_at_nanos) {
  engine.on_market_data(instrument_id, price, observed_at_nanos, /*valid=*/true);
}

OrderRequest make_request(std::uint32_t instrument_id, Side side, std::int64_t price,
                          std::int64_t quantity, Nanos now,
                          const std::string& strategy_id = "strat",
                          const std::string& proposal_id = "p1", std::uint32_t leg_index = 0) {
  return OrderRequest{.strategy_id = strategy_id,
                      .proposal_id = proposal_id,
                      .leg_index = leg_index,
                      .instrument_id = instrument_id,
                      .side = side,
                      .price_units = price,
                      .quantity_units = quantity,
                      .request_time_nanos = now};
}

// ---------------------------------------------------------------- AEGIS-121

TEST(RiskEngineOrderQuantity, RejectsAboveLimitWhenResizeDisabled) {
  RiskLimitsConfig config = base_config();
  config.order_quantity_limits[kNear] =
      OrderQuantityLimit{.max_order_quantity_units = 10, .resize_on_breach = false};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 9, 0)).verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 10, 0)).verdict,
            RiskVerdict::kApprove);
  const LegDecision over = engine.evaluate(make_request(kNear, Side::kBuy, 100, 11, 0));
  EXPECT_EQ(over.verdict, RiskVerdict::kReject);
  EXPECT_EQ(over.reason_code, ReasonCode::kMaxOrderQuantity);
}

TEST(RiskEngineOrderQuantity, ResizesDownToTheLimitWhenResizeEnabled) {
  RiskLimitsConfig config = base_config();
  config.order_quantity_limits[kNear] =
      OrderQuantityLimit{.max_order_quantity_units = 10, .resize_on_breach = true};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  const LegDecision decision = engine.evaluate(make_request(kNear, Side::kBuy, 100, 25, 0));
  EXPECT_EQ(decision.verdict, RiskVerdict::kResize);
  EXPECT_EQ(decision.approved_quantity_units, 10);
}

// ------------------------------------------------------- AEGIS-121/122 (R1)
//
// M5 closure repair: quantity_units is a positive MAGNITUDE; Side alone
// carries direction. A negative or zero quantity used to be treated as
// legitimate input all the way into a reservation, which could go negative
// and then SUBTRACT from every cumulative control's projected exposure.

TEST(RiskEngineQuantityValidity, NegativeQuantityIsRejectedAsInvalid) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100, 0);
  const LegDecision decision = engine.evaluate(make_request(kNear, Side::kBuy, 100, -1, 0));
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kInvalidQuantity);
}

TEST(RiskEngineQuantityValidity, ZeroQuantityIsRejectedAsInvalid) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100, 0);
  const LegDecision decision = engine.evaluate(make_request(kNear, Side::kBuy, 100, 0, 0));
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kInvalidQuantity);
}

TEST(RiskEngineQuantityValidity, OnePositiveUnitProceedsPastTheQuantityCheck) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100, 0);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).verdict,
            RiskVerdict::kApprove);
}

TEST(RiskEngineQuantityValidity, PositiveBoundaryAtAndAboveMaxOrderQuantityIsUnaffected) {
  RiskLimitsConfig config = base_config();
  config.order_quantity_limits[kNear] =
      OrderQuantityLimit{.max_order_quantity_units = 100, .resize_on_breach = false};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 100, 0)).verdict,
            RiskVerdict::kApprove);
  const LegDecision over = engine.evaluate(make_request(kNear, Side::kBuy, 100, 101, 0));
  EXPECT_EQ(over.verdict, RiskVerdict::kReject);
  // Over-cap is still kMaxOrderQuantity, never reinterpreted as kInvalidQuantity.
  EXPECT_EQ(over.reason_code, ReasonCode::kMaxOrderQuantity);
}

TEST(RiskEngineQuantityValidity,
     NegativeQuantityCommitCreatesNoReservationAndCannotBypassThePositionCap) {
  // The reviewer's exact R1 attack: a 100-lot position cap, a
  // negative-quantity leg committed first (which, before this fix, drove a
  // negative reserved total that then SUBTRACTED from later projections),
  // followed by a legitimate 150-lot order that must still be rejected by
  // the same cap.
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  const std::vector<OrderRequest> negative_leg{
      make_request(kNear, Side::kBuy, 100, -100, 0, "attacker", "p-negative", 0)};
  const auto& decision = engine.commit_proposal_decision("attacker", "p-negative", negative_leg, 0);
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kInvalidQuantity);
  EXPECT_EQ(engine.state().reserved_units(kNear), 0);  // No reservation of any sign was created.

  const LegDecision legitimate = engine.evaluate(make_request(kNear, Side::kBuy, 100, 150, 0));
  EXPECT_EQ(legitimate.verdict, RiskVerdict::kReject);
  EXPECT_EQ(legitimate.reason_code, ReasonCode::kMaxPositionLong);
}

TEST(RiskEngineQuantityValidity,
     UnboundedNegativeQuantityIsRejectedEvenWithNoConfiguredPositionLimit) {
  // No position_limits entry at all for kNear: the old bug let an
  // arbitrarily large negative reservation through with nothing configured
  // to catch it.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100, 0);
  const LegDecision decision = engine.evaluate(make_request(kNear, Side::kBuy, 100, -1'000'000, 0));
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kInvalidQuantity);
}

TEST(RiskEngineQuantityValidity,
     MultiLegProposalWithOneInvalidLegRejectsTheWholeProposalAtomically) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100, 0);
  seed_valid_quote(engine, kFar, 100, 0);

  const std::string proposal_id = "spread-invalid";
  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kSell, 100, 5, 0, "spread_strategy", proposal_id, 0),
      make_request(kFar, Side::kBuy, 100, 0, 0, "spread_strategy", proposal_id, 1),  // invalid
  };
  const auto& decision = engine.commit_proposal_decision("spread_strategy", proposal_id, legs, 0);
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kInvalidQuantity);
  ASSERT_EQ(decision.legs.size(), 2U);
  for (const auto& leg : decision.legs) {
    EXPECT_EQ(leg.verdict, RiskVerdict::kReject);
  }
  EXPECT_EQ(engine.state().reserved_units(kNear), 0);  // Neither leg reserved anything.
  EXPECT_EQ(engine.state().reserved_units(kFar), 0);
}

// ------------------------------------------------------------------- N2
//
// A malformed CONFIGURED order-quantity cap (non-positive, with
// resize_on_breach) can make resolve_effective_quantity itself manufacture
// a non-positive approved quantity. check_approved_quantity_postcondition
// is the defense-in-depth backstop for exactly that -- proven here by
// constructing the malformed RiskLimitsConfig directly (programmatically),
// which load_risk_limits_config's own loader-side validation cannot see.

TEST(RiskEngineQuantityValidity, MalformedNegativeConfiguredCapNeverProducesANonPositiveApproval) {
  RiskLimitsConfig config = base_config();
  config.order_quantity_limits[kNear] =
      OrderQuantityLimit{.max_order_quantity_units = -100, .resize_on_breach = true};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  const LegDecision decision = engine.evaluate(make_request(kNear, Side::kBuy, 100, 150, 0));
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kInvalidLimitConfiguration);
  EXPECT_EQ(engine.state().reserved_units(kNear), 0);
}

TEST(RiskEngineQuantityValidity, MalformedZeroConfiguredCapNeverProducesANonPositiveApproval) {
  RiskLimitsConfig config = base_config();
  config.order_quantity_limits[kNear] =
      OrderQuantityLimit{.max_order_quantity_units = 0, .resize_on_breach = true};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  const LegDecision decision = engine.evaluate(make_request(kNear, Side::kBuy, 100, 150, 0));
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kInvalidLimitConfiguration);
}

TEST(RiskEngineQuantityValidity, AWellFormedResizeIsUnaffectedByThePostcondition) {
  // Regression: the new postcondition must not interfere with an ordinary,
  // well-formed resize (existing valid-resize behavior stays intact).
  RiskLimitsConfig config = base_config();
  config.order_quantity_limits[kNear] =
      OrderQuantityLimit{.max_order_quantity_units = 100, .resize_on_breach = true};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  const LegDecision decision = engine.evaluate(make_request(kNear, Side::kBuy, 100, 150, 0));
  EXPECT_EQ(decision.verdict, RiskVerdict::kResize);
  EXPECT_EQ(decision.approved_quantity_units, 100);
}

TEST(RiskEngineQuantityValidity,
     MultiLegProposalWithAMalformedConfiguredCapRejectsTheWholeProposalAtomically) {
  RiskLimitsConfig config = base_config();
  config.order_quantity_limits[kNear] =
      OrderQuantityLimit{.max_order_quantity_units = -1, .resize_on_breach = true};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);
  seed_valid_quote(engine, kFar, 100, 0);

  const std::string proposal_id = "spread-bad-config";
  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kBuy, 100, 10, 0, "spread_strategy", proposal_id, 0),
      make_request(kFar, Side::kSell, 100, 10, 0, "spread_strategy", proposal_id, 1),
  };
  const auto& decision = engine.commit_proposal_decision("spread_strategy", proposal_id, legs, 0);
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kInvalidLimitConfiguration);
  EXPECT_EQ(engine.state().reserved_units(kNear), 0);
  EXPECT_EQ(engine.state().reserved_units(kFar), 0);
}

// ------------------------------------------------------------------- N3
//
// A fill quantity is also a positive magnitude; side carries direction. An
// invalid fill must mutate nothing -- no position, no reservation.

TEST(RiskEngineQuantityValidity, NegativeFillMutatesNeitherPositionNorReservation) {
  RiskLimitsConfig config = base_config();
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  ASSERT_EQ(engine
                .commit_proposal_decision(
                    "strat", "fill-p1",
                    {make_request(kNear, Side::kBuy, 100, 100, 0, "strat", "fill-p1")}, 0)
                .verdict,
            RiskVerdict::kApprove);
  const auto decision = decide_registered_order(engine, "strat", "fill-p1", 0, kNear, Side::kBuy,
                                                100, /*client_order_id=*/1, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);
  ASSERT_EQ(engine.state().reserved_units(kNear), 100);
  ASSERT_EQ(engine.state().position_units(kNear), 0);

  engine.on_fill(/*client_order_id=*/1, kNear, Side::kBuy, -50);
  EXPECT_EQ(engine.state().reserved_units(kNear), 100);  // Unchanged.
  EXPECT_EQ(engine.state().position_units(kNear), 0);    // Unchanged.
}

TEST(RiskEngineQuantityValidity, ZeroFillMutatesNeitherPositionNorReservation) {
  RiskLimitsConfig config = base_config();
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  ASSERT_EQ(engine
                .commit_proposal_decision(
                    "strat", "fill-p2",
                    {make_request(kNear, Side::kBuy, 100, 100, 0, "strat", "fill-p2")}, 0)
                .verdict,
            RiskVerdict::kApprove);
  const auto decision = decide_registered_order(engine, "strat", "fill-p2", 0, kNear, Side::kBuy,
                                                100, /*client_order_id=*/1, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);

  engine.on_fill(/*client_order_id=*/1, kNear, Side::kBuy, 0);
  EXPECT_EQ(engine.state().reserved_units(kNear), 100);  // Unchanged.
  EXPECT_EQ(engine.state().position_units(kNear), 0);    // Unchanged.
}

TEST(RiskEngineQuantityValidity, PositiveFillBehavesExactlyAsBefore) {
  RiskLimitsConfig config = base_config();
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  ASSERT_EQ(engine
                .commit_proposal_decision(
                    "strat", "fill-p3",
                    {make_request(kNear, Side::kBuy, 100, 100, 0, "strat", "fill-p3")}, 0)
                .verdict,
            RiskVerdict::kApprove);
  const auto decision = decide_registered_order(engine, "strat", "fill-p3", 0, kNear, Side::kBuy,
                                                100, /*client_order_id=*/1, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);

  engine.on_fill(/*client_order_id=*/1, kNear, Side::kBuy, 60);
  EXPECT_EQ(engine.state().reserved_units(kNear), 40);
  EXPECT_EQ(engine.state().position_units(kNear), 60);
}

TEST(RiskEngineQuantityValidity, PositiveOverfillStillSaturatesReservationAtZeroPerR2) {
  // Regression: N3's validation must not disturb R2's over-fill clamp for a
  // genuinely positive (just too large) fill.
  RiskLimitsConfig config = base_config();
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  ASSERT_EQ(engine
                .commit_proposal_decision(
                    "strat", "fill-p4",
                    {make_request(kNear, Side::kBuy, 100, 100, 0, "strat", "fill-p4")}, 0)
                .verdict,
            RiskVerdict::kApprove);
  const auto decision = decide_registered_order(engine, "strat", "fill-p4", 0, kNear, Side::kBuy,
                                                100, /*client_order_id=*/1, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);

  engine.on_fill(/*client_order_id=*/1, kNear, Side::kBuy, 130);
  EXPECT_EQ(engine.state().reserved_units(kNear), 0);    // Saturated, not negative (R2).
  EXPECT_EQ(engine.state().position_units(kNear), 130);  // The actual fill still applies in full.
}

// ---------------------------------------------------------------- AEGIS-122

TEST(RiskEnginePositionLimits, RejectsBeyondLongAndShortBoundaries) {
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 100, 0)).verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 101, 0)).reason_code,
            ReasonCode::kMaxPositionLong);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kSell, 100, 100, 0)).verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kSell, 100, 101, 0)).reason_code,
            ReasonCode::kMaxPositionShort);
}

TEST(RiskEnginePositionLimits, TwoIndividuallyAcceptableOrdersJointlyBreach) {
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  // Each order alone is within the limit (60 <= 100), but reserving the
  // first must make the second's projected position (120) breach.
  const LegDecision first = engine.evaluate(make_request(kNear, Side::kBuy, 100, 60, 0));
  ASSERT_EQ(first.verdict, RiskVerdict::kApprove);
  const auto& decision = engine.commit_proposal_decision(
      "strat", "reserve-1", {make_request(kNear, Side::kBuy, 100, 60, 0, "strat", "reserve-1")}, 0);
  ASSERT_EQ(decision.verdict, RiskVerdict::kApprove);
  const auto oms_decision =
      decide_registered_order(engine, "strat", "reserve-1", /*leg_index=*/0, kNear, Side::kBuy, 60,
                              /*client_order_id=*/1, 0);
  ASSERT_EQ(oms_decision.verdict, RiskVerdict::kApprove);

  const LegDecision second = engine.evaluate(make_request(kNear, Side::kBuy, 100, 60, 0));
  EXPECT_EQ(second.verdict, RiskVerdict::kReject);
  EXPECT_EQ(second.reason_code, ReasonCode::kMaxPositionLong);
}

TEST(RiskEnginePositionLimits, PartialFillCancelAndRejectAllReleaseTheReservation) {
  RiskLimitsConfig config = base_config();
  config.position_limits[kNear] = PositionLimit{.max_long_units = 100, .max_short_units = 100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  auto reserve = [&](std::uint64_t client_order_id, const std::string& proposal_id) {
    engine.commit_proposal_decision(
        "strat", proposal_id, {make_request(kNear, Side::kBuy, 100, 100, 0, "strat", proposal_id)},
        0);
    return decide_registered_order(engine, "strat", proposal_id, /*leg_index=*/0, kNear, Side::kBuy,
                                   100, client_order_id, 0);
  };

  // Cancel releases fully.
  ASSERT_EQ(reserve(1, "p-cancel").verdict, RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).reason_code,
            ReasonCode::kMaxPositionLong);
  engine.on_order_terminated(1);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 100, 0)).verdict,
            RiskVerdict::kApprove);

  // Downstream rejection releases fully.
  ASSERT_EQ(reserve(2, "p-reject").verdict, RiskVerdict::kApprove);
  engine.on_order_rejected(2);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 100, 0)).verdict,
            RiskVerdict::kApprove);

  // A partial fill converts reserved exposure into confirmed position --
  // the projected total is unchanged, only its composition is.
  ASSERT_EQ(reserve(3, "p-partial").verdict, RiskVerdict::kApprove);
  engine.on_fill(3, kNear, Side::kBuy, 40);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).reason_code,
            ReasonCode::kMaxPositionLong);
  engine.on_order_terminated(3);  // Remaining 60 released; 40 stays as confirmed position.
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 60, 0)).verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 61, 0)).reason_code,
            ReasonCode::kMaxPositionLong);

  // The general release primitive (documented stand-in for a failed adapter
  // submission the OMS seam does not itself expose -- docs/LIMITATIONS.md).
  // 40 units are already confirmed position from the partial-fill above, so
  // this reservation is sized to the remaining 60-unit budget.
  engine.commit_proposal_decision(
      "strat", "p-explicit-release",
      {make_request(kNear, Side::kBuy, 100, 60, 0, "strat", "p-explicit-release")}, 0);
  ASSERT_EQ(decide_registered_order(engine, "strat", "p-explicit-release", /*leg_index=*/0, kNear,
                                    Side::kBuy, 60, /*client_order_id=*/4, 0)
                .verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).reason_code,
            ReasonCode::kMaxPositionLong);
  engine.release_reservation(4);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 60, 0)).verdict,
            RiskVerdict::kApprove);
}

// ---------------------------------------------------------------- AEGIS-123

TEST(RiskEngineNotional, RejectsAboveOrderAndPortfolioNotional) {
  RiskLimitsConfig config = base_config();
  config.max_order_notional_units = 1'000;
  config.max_portfolio_notional_units = 1'500;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 10, 0)).verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 11, 0)).reason_code,
            ReasonCode::kMaxOrderNotional);
}

TEST(RiskEngineNotional, UnsupportedCurrencyIsRejectedExplicitly) {
  RiskLimitsConfig config = base_config();
  config.instruments[kNear].currency = "EUR";  // No fx_rate_to_base entry for EUR.
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  const LegDecision decision = engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0));
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.reason_code, ReasonCode::kUnsupportedCurrency);
}

TEST(RiskEngineNotional, SupportedNonBaseCurrencyIsNormalizedByFxRate) {
  RiskLimitsConfig config = base_config();
  config.instruments[kNear].currency = "EUR";
  config.fx_rate_to_base["EUR"] = 1.1;
  config.max_order_notional_units = 200;  // In base (USD) units.
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  // 100 * 1 * 1.1 = 110 <= 200: approved.
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).verdict,
            RiskVerdict::kApprove);
  // 100 * 2 * 1.1 = 220 > 200: rejected.
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 2, 0)).reason_code,
            ReasonCode::kMaxOrderNotional);
}

// ---------------------------------------------------------------- AEGIS-124

TEST(RiskEngineExposure, RejectsAboveMarketAndSectorLimits) {
  RiskLimitsConfig config = base_config();
  config.market_exposure_limit_units["EQX"] = 500;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);
  seed_valid_quote(engine, kFar, 100, 0);

  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 5, 0)).verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 6, 0)).reason_code,
            ReasonCode::kMarketExposure);
}

// ---------------------------------------------------------------- AEGIS-125

TEST(RiskEnginePriceCollar, BoundaryNoReferenceAndStaleReferenceCases) {
  RiskLimitsConfig config = base_config();
  config.price_collar_bps = 500;  // 5%.
  RiskEngine engine(config);

  // No reference price yet.
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).reason_code,
            ReasonCode::kNoReferencePrice);

  seed_valid_quote(engine, kNear, 100'000, 0);
  // Exactly at the 5% collar: 105'000 is allowed.
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 105'000, 1, 0)).verdict,
            RiskVerdict::kApprove);
  // Beyond the collar.
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 105'001, 1, 0)).reason_code,
            ReasonCode::kPriceCollar);

  // An invalid update never becomes the new reference: the collar is still
  // judged against the last VALID price (100'000), not the invalid one.
  engine.on_market_data(kNear, 999'999, 1, /*valid=*/false);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 105'000, 1, 1)).verdict,
            RiskVerdict::kApprove);
}

// ---------------------------------------------------------------- AEGIS-126

TEST(RiskEngineStaleData, RejectsStaleAndInvalidMarketState) {
  RiskLimitsConfig config = base_config();
  config.max_quote_age = Duration{100};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, /*observed_at_nanos=*/0);

  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, /*now=*/50)).verdict,
            RiskVerdict::kApprove);
  const LegDecision stale = engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, /*now=*/200));
  EXPECT_EQ(stale.verdict, RiskVerdict::kReject);
  EXPECT_EQ(stale.reason_code, ReasonCode::kStaleMarketData);

  // A structurally invalid update (non-positive price) is never recorded as
  // a valid reference at all -- evaluating against it still sees no
  // reference price for an instrument that never got one.
  RiskEngine fresh_engine(config);
  fresh_engine.on_market_data(kFar, -5, 0, /*valid=*/true);
  EXPECT_EQ(fresh_engine.evaluate(make_request(kFar, Side::kBuy, 100, 1, 0)).reason_code,
            ReasonCode::kNoReferencePrice);
}

// ---------------------------------------------------------------- AEGIS-127

TEST(RiskEngineIdempotency, ReplayedProposalReturnsTheSameDecisionAndNeverDoubles) {
  // M5 closure repair (AEGIS-137): a replayed proposal_id must not create a
  // SECOND ProposalRiskDecision -- an independent risk-safety review found
  // the previous version did exactly that (the per-leg dedupe check
  // rejected the replay's legs, but commit_proposal_decision still
  // unconditionally appended a second, reject, record for the same
  // proposal_id -- and this very test never asserted the count, so it
  // passed anyway). The fix returns the ORIGINAL terminal decision
  // unchanged on replay, so proposal_decision_count can never exceed 1.
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100, 0);

  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kBuy, 100, 1, 0, "strat", "dup-1")};
  const auto& first = engine.commit_proposal_decision("strat", "dup-1", legs, 0);
  EXPECT_EQ(first.verdict, RiskVerdict::kApprove);
  const std::uint64_t first_sequence = first.sequence;

  const auto& replay = engine.commit_proposal_decision("strat", "dup-1", legs, 1);
  EXPECT_EQ(replay.sequence, first_sequence);  // The SAME record, not a new one.
  EXPECT_EQ(replay.verdict, RiskVerdict::kApprove);
  EXPECT_EQ(engine.audit_log().proposal_decision_count("dup-1"), 1U);

  // AEGIS-127's actual substance -- a replay consumes no NEW risk capacity --
  // still holds: the replay armed no second pending leg, so the one leg
  // "dup-1" ever armed is still there, resolvable exactly once.
  EXPECT_EQ(engine.state().leg_reservation_count(), 1U);
}

// ---------------------------------------------------------------- AEGIS-128

TEST(RiskEngineRateLimit, ThrottlesOrdersAndCancelsOnAVirtualClock) {
  RiskLimitsConfig config = base_config();
  config.rate_limit_window = Duration{1'000};
  config.max_orders_per_window = 2;
  config.max_cancels_per_window = 1;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  engine.commit_proposal_decision("s", "r1",
                                  {make_request(kNear, Side::kBuy, 100, 1, 0, "s", "r1")}, 0);
  engine.commit_proposal_decision("s", "r2",
                                  {make_request(kNear, Side::kBuy, 100, 1, 10, "s", "r2")}, 10);
  const auto& third = engine.commit_proposal_decision(
      "s", "r3", {make_request(kNear, Side::kBuy, 100, 1, 20, "s", "r3")}, 20);
  EXPECT_EQ(third.verdict, RiskVerdict::kReject);
  EXPECT_EQ(third.reason_code, ReasonCode::kMessageRateLimit);

  // After the window rolls forward, a new order is allowed again.
  const auto& fourth = engine.commit_proposal_decision(
      "s", "r4", {make_request(kNear, Side::kBuy, 100, 1, 2'000, "s", "r4")}, 2'000);
  EXPECT_NE(fourth.verdict, RiskVerdict::kReject);

  // Cancel rate limiting is independent of the order limit, and a
  // safety-driven cancel always bypasses it (the critical safety rule).
  EXPECT_TRUE(engine.allow_cancel(0));
  EXPECT_FALSE(engine.allow_cancel(1));
  EXPECT_TRUE(engine.allow_cancel(1, /*bypass_safety=*/true));
}

// ---------------------------------------------------------------- AEGIS-129/130

TEST(RiskEngineMarginAndLeverage, RejectsInsufficientMargin) {
  RiskLimitsConfig config = base_config();
  config.margin.margin_per_contract_units[kNear] = 100;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 10, 0);
  engine.on_equity_update(500);

  // 5 contracts * 100 = 500 <= 500 equity (boundary, ok); 6 * 100 = 600 > 500.
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 10, 5, 0)).verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 10, 6, 0)).reason_code,
            ReasonCode::kInsufficientMargin);
}

TEST(RiskEngineMarginAndLeverage, RejectsExcessiveLeverage) {
  RiskLimitsConfig config = base_config();
  config.max_leverage = 2.0;  // No margin config here: isolates the leverage check.
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 10, 0);
  engine.on_equity_update(500);

  // notional = price(10) * qty * multiplier(1); equity = 500;
  // max_leverage 2.0 -> notional <= 1000 -> qty <= 100.
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 10, 100, 0)).verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 10, 101, 0)).reason_code,
            ReasonCode::kMaxLeverage);

  // Non-positive equity cannot support any leverage.
  RiskEngine zero_equity_engine(config);
  seed_valid_quote(zero_equity_engine, kNear, 10, 0);
  zero_equity_engine.on_equity_update(0);
  EXPECT_EQ(zero_equity_engine.evaluate(make_request(kNear, Side::kBuy, 10, 1, 0)).reason_code,
            ReasonCode::kMaxLeverage);
}

// ---------------------------------------------------------------- AEGIS-131

TEST(RiskEngineDailyLoss, TripsExactlyOnceAndLatchesTrading) {
  RiskLimitsConfig config = base_config();
  config.daily_loss_limit_units = 1'000;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).verdict,
            RiskVerdict::kApprove);
  engine.on_session_pnl_update(-1'000);  // Trips exactly at the threshold.
  EXPECT_TRUE(engine.state().is_daily_loss_tripped());
  const LegDecision after = engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0));
  EXPECT_EQ(after.verdict, RiskVerdict::kReject);
  EXPECT_EQ(after.reason_code, ReasonCode::kTradingHalted);

  // Deeper losses do not "trip again" -- the latch is idempotent by
  // construction (trip_daily_loss returns false once already tripped).
  engine.on_session_pnl_update(-5'000);
  EXPECT_TRUE(engine.state().is_daily_loss_tripped());

  // A new session clears the daily-loss latch only.
  engine.start_new_session();
  EXPECT_FALSE(engine.state().is_daily_loss_tripped());
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).verdict,
            RiskVerdict::kApprove);
}

// ---------------------------------------------------------------- AEGIS-132

TEST(RiskEngineDrawdown, HighWaterMarkScenariosTripTheLatch) {
  RiskLimitsConfig config = base_config();
  config.max_drawdown_units = 50;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);

  engine.on_equity_update(1'000);  // Initial high-water mark.
  engine.on_equity_update(970);    // Drawdown 30: within limit.
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).verdict,
            RiskVerdict::kApprove);

  engine.on_equity_update(940);  // Drawdown 60: breach.
  EXPECT_TRUE(engine.state().is_drawdown_tripped());
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0)).reason_code,
            ReasonCode::kTradingHalted);

  // Recovery does not un-trip the latch (drawdown is a max-ever quantity,
  // not a start-of-session one) -- start_new_session deliberately does not
  // clear it either.
  engine.on_equity_update(1'100);  // New high-water mark past the old one.
  engine.start_new_session();
  EXPECT_TRUE(engine.state().is_drawdown_tripped());
}

// ---------------------------------------------------------------- AEGIS-133

TEST(RiskEngineVolatilityReduction, ResizesDownAsVolatilityRisesAndRejectsBeyondHardMultiple) {
  RiskLimitsConfig config = base_config();
  config.volatility = VolatilityReductionConfig{
      .window = 5, .target_volatility = 0.01, .hard_reject_multiple = 3.0};
  RiskEngine engine(config);

  // Feed a return series whose realized volatility lands strictly between
  // target_volatility (0.01) and hard_reject_multiple * target (0.03): small
  // enough to resize, not reject outright.
  std::int64_t price = 1'000;
  for (int i = 0; i < 6; ++i) {
    price += (i % 2 == 0) ? 15 : -15;  // Alternating ~+-1.5% swings.
    engine.on_market_data(kNear, price, i, true);
  }
  const LegDecision decision = engine.evaluate(make_request(kNear, Side::kBuy, price, 100, 6));
  ASSERT_EQ(decision.verdict, RiskVerdict::kResize);
  EXPECT_LT(decision.approved_quantity_units, 100);
  EXPECT_GE(decision.approved_quantity_units, 1);
}

// ---------------------------------------------------------------- AEGIS-134

TEST(RiskEngineConcentration, RejectsCorrelatedGroupExposureBreach) {
  RiskLimitsConfig config = base_config();
  config.concentration.correlated_groups["pair"] = {kNear, kFar};
  config.concentration.group_exposure_limit_units["pair"] = 500;
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);
  seed_valid_quote(engine, kFar, 100, 0);

  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 5, 0)).verdict,
            RiskVerdict::kApprove);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 6, 0)).reason_code,
            ReasonCode::kCorrelatedExposure);
}

// ---------------------------------------------------------------- AEGIS-137

TEST(RiskEngineAuditInvariant, ExactlyOneProposalDecisionAndOneOrderDecisionPerLeg) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100, 0);
  seed_valid_quote(engine, kFar, 100, 0);

  const std::string proposal_id = "spread-1";
  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kSell, 100, 1, 0, "spread_strategy", proposal_id, 0),
      make_request(kFar, Side::kBuy, 100, 1, 0, "spread_strategy", proposal_id, 1),
  };
  engine.commit_proposal_decision("spread_strategy", proposal_id, legs, 0);
  EXPECT_EQ(engine.audit_log().proposal_decision_count(proposal_id), 1U);

  // Both constituents staged and the proposal authorized ONCE, before either
  // leg is released (ADR-0027 "Correction 3").
  ASSERT_EQ(
      aegis::testing::stage_and_authorize(engine, "spread_strategy", proposal_id,
                                          {aegis::testing::staged_leg(1, 0, kNear, Side::kSell, 1),
                                           aegis::testing::staged_leg(2, 1, kFar, Side::kBuy, 1)})
          .state,
      aegis::participant::risk::ProposalReleaseState::kAuthorizedForRelease);
  EXPECT_EQ(engine.audit_log().proposal_release_decision_count(proposal_id), 1U);

  static_cast<void>(engine.decide_order(kNear, Side::kSell, 1, /*client_order_id=*/1, 0));
  static_cast<void>(engine.decide_order(kFar, Side::kBuy, 1, /*client_order_id=*/2, 0));
  EXPECT_EQ(engine.audit_log().order_decisions().size(), 2U);
  for (const auto& order_decision : engine.audit_log().order_decisions()) {
    EXPECT_EQ(order_decision.proposal_id, proposal_id);
  }
}

TEST(RiskEngineAuditInvariant, RejectedProposalReportsEveryLegRejectedAndArmsNoOrder) {
  RiskLimitsConfig config = base_config();
  config.order_quantity_limits[kNear] =
      OrderQuantityLimit{.max_order_quantity_units = 0, .resize_on_breach = false};
  RiskEngine engine(config);
  seed_valid_quote(engine, kNear, 100, 0);
  seed_valid_quote(engine, kFar, 100, 0);

  const std::string proposal_id = "spread-reject";
  const std::vector<OrderRequest> legs{
      make_request(kNear, Side::kSell, 100, 1, 0, "s", proposal_id, 0),
      make_request(kFar, Side::kBuy, 100, 1, 0, "s", proposal_id, 1),
  };
  const auto& decision = engine.commit_proposal_decision("s", proposal_id, legs, 0);
  EXPECT_EQ(decision.verdict, RiskVerdict::kReject);
  ASSERT_EQ(decision.legs.size(), 2U);
  EXPECT_EQ(decision.legs[0].verdict, RiskVerdict::kReject);
  EXPECT_EQ(decision.legs[1].verdict,
            RiskVerdict::kReject);  // The far leg, never itself over-quantity.

  // Because nothing was armed, an order that tries to reach the seam anyway
  // (a bypass, or a caller bug) is rejected as unexpected -- the structural
  // defence AEGIS-120 depends on.
  const auto oms_decision = engine.decide_order(kFar, Side::kBuy, 1, 99, 0);
  EXPECT_EQ(oms_decision.verdict, RiskVerdict::kReject);
  EXPECT_EQ(oms_decision.reason_code, ReasonCode::kUnexpectedOrder);
}

// ---------------------------------------------------------------- AEGIS-135

TEST(RiskEngineKillSwitch, StrategyAndGlobalTripsAreIdempotentAndScopedCorrectly) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100, 0);

  EXPECT_TRUE(engine.trip_strategy("strat-a"));
  EXPECT_FALSE(engine.trip_strategy("strat-a"));  // Second trip is a no-op.
  EXPECT_TRUE(engine.is_strategy_halted("strat-a"));
  EXPECT_FALSE(engine.is_strategy_halted("strat-b"));  // Unrelated strategy unaffected.

  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0, "strat-a")).reason_code,
            ReasonCode::kKillSwitchStrategy);
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0, "strat-b")).verdict,
            RiskVerdict::kApprove);

  EXPECT_TRUE(engine.trip_global());
  EXPECT_FALSE(engine.trip_global());
  EXPECT_TRUE(engine.is_globally_halted());
  EXPECT_EQ(engine.evaluate(make_request(kNear, Side::kBuy, 100, 1, 0, "strat-b")).reason_code,
            ReasonCode::kKillSwitchGlobal);
}

// ---------------------------------------------------------------- AEGIS-136

TEST(RiskEngineConnectivity, DisconnectsEachBlockNewOrdersUntilReconnected) {
  RiskEngine engine(base_config());
  seed_valid_quote(engine, kNear, 100, 0);
  const OrderRequest request = make_request(kNear, Side::kBuy, 100, 1, 0);

  engine.on_feed_disconnected();
  EXPECT_EQ(engine.evaluate(request).reason_code, ReasonCode::kFeedDisconnected);
  engine.on_feed_reconnected();

  engine.on_exchange_disconnected();
  EXPECT_EQ(engine.evaluate(request).reason_code, ReasonCode::kExchangeDisconnected);
  engine.on_exchange_reconnected();

  engine.on_broker_disconnected();
  EXPECT_EQ(engine.evaluate(request).reason_code, ReasonCode::kBrokerDisconnected);
  engine.on_broker_reconnected();

  EXPECT_EQ(engine.evaluate(request).verdict, RiskVerdict::kApprove);
}

}  // namespace
