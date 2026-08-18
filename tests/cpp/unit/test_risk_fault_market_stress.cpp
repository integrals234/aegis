#include <gtest/gtest.h>

#include "cpp/participant/app/fault_scenario.hpp"
#include "cpp/participant/risk/risk_engine.hpp"

/// AEGIS-062 M5 residual: reproducible RISK RESPONSES to the three market-
/// stress fault kinds M2 already delivers deterministically
/// (`test_market_stress_faults.cpp`). Each response here is a real
/// `risk::RiskEngine::evaluate` call, not a report generated after the
/// fact -- see `run_market_stress_risk_scenario`'s doc comment for the
/// exact translation each kind gets.
namespace {

using aegis::participant::app::run_market_stress_risk_scenario;
using aegis::participant::risk::InstrumentInfo;
using aegis::participant::risk::ReasonCode;
using aegis::participant::risk::RiskEngine;
using aegis::participant::risk::RiskLimitsConfig;
using aegis::participant::risk::RiskVerdict;
using aegis::replay::FaultKind;
using aegis::replay::FaultRule;
using aegis::replay::RecordIndex;

constexpr std::uint32_t kInstrumentId = 5001;
constexpr std::int64_t kBasePrice = 100'000;

RiskLimitsConfig config_for(std::int64_t price_collar_bps, double target_volatility,
                            aegis::common::Duration max_quote_age) {
  RiskLimitsConfig config;
  config.instruments[kInstrumentId] =
      InstrumentInfo{.multiplier_units = 1, .currency = "USD", .market = "", .sector = ""};
  config.price_collar_bps = price_collar_bps;
  config.volatility.window = 5;
  config.volatility.target_volatility = target_volatility;
  config.max_quote_age = max_quote_age;
  return config;
}

TEST(MarketStressRisk, SpreadWideningTripsThePriceCollar) {
  RiskEngine engine(
      config_for(/*price_collar_bps=*/500, /*target_volatility=*/0.0, aegis::common::Duration{0}));
  const std::vector<FaultRule> rules{{.target = RecordIndex{0},
                                      .kind = FaultKind::kSpreadWidening,
                                      .delay = {},
                                      .magnitude = 1'000}};

  const auto responses = run_market_stress_risk_scenario(engine, kInstrumentId, kBasePrice,
                                                         /*order_quantity_units=*/1, rules, 0);
  ASSERT_EQ(responses.size(), 1U);
  EXPECT_EQ(responses[0].kind, FaultKind::kSpreadWidening);
  EXPECT_EQ(responses[0].verdict, RiskVerdict::kReject);
  EXPECT_EQ(responses[0].reason_code, ReasonCode::kPriceCollar);
}

TEST(MarketStressRisk, SpreadWideningWithinCollarStillApproves) {
  RiskEngine engine(config_for(/*price_collar_bps=*/500, 0.0, aegis::common::Duration{0}));
  const std::vector<FaultRule> rules{{.target = RecordIndex{0},
                                      .kind = FaultKind::kSpreadWidening,
                                      .delay = {},
                                      .magnitude = 100}};

  const auto responses =
      run_market_stress_risk_scenario(engine, kInstrumentId, kBasePrice, 1, rules, 0);
  ASSERT_EQ(responses.size(), 1U);
  EXPECT_EQ(responses[0].verdict, RiskVerdict::kApprove);
}

TEST(MarketStressRisk, VolatilitySpikeResizesOrRejects) {
  RiskEngine engine(
      config_for(/*price_collar_bps=*/0, /*target_volatility=*/0.01, aegis::common::Duration{0}));
  const std::vector<FaultRule> rules{{.target = RecordIndex{0},
                                      .kind = FaultKind::kVolatilitySpike,
                                      .delay = {},
                                      .magnitude = 150}};

  const auto responses =
      run_market_stress_risk_scenario(engine, kInstrumentId, kBasePrice, 100, rules, 0);
  ASSERT_EQ(responses.size(), 1U);
  EXPECT_EQ(responses[0].kind, FaultKind::kVolatilitySpike);
  // A resize carries no reason_code (kNone) -- only a hard reject does;
  // resizing down as volatility rises, not rejecting outright, is exactly
  // AEGIS-133's "resize OR reject" contract for a spike within
  // hard_reject_multiple of target.
  EXPECT_EQ(responses[0].verdict, RiskVerdict::kResize);
}

TEST(MarketStressRisk, LiquidityVanishTripsStaleness) {
  RiskEngine engine(
      config_for(/*price_collar_bps=*/0, /*target_volatility=*/0.0, aegis::common::Duration{100}));
  const std::vector<FaultRule> rules{{.target = RecordIndex{0},
                                      .kind = FaultKind::kLiquidityVanish,
                                      .delay = {},
                                      .magnitude = 1'000}};

  const auto responses =
      run_market_stress_risk_scenario(engine, kInstrumentId, kBasePrice, 1, rules, 0);
  ASSERT_EQ(responses.size(), 1U);
  EXPECT_EQ(responses[0].kind, FaultKind::kLiquidityVanish);
  EXPECT_EQ(responses[0].verdict, RiskVerdict::kReject);
  EXPECT_EQ(responses[0].reason_code, ReasonCode::kStaleMarketData);
}

TEST(MarketStressRisk, ResponsesAreReproducibleAcrossIndependentEngines) {
  const RiskLimitsConfig config = config_for(500, 0.01, aegis::common::Duration{100});
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{0},
       .kind = FaultKind::kSpreadWidening,
       .delay = {},
       .magnitude = 1'000},
      {.target = RecordIndex{1},
       .kind = FaultKind::kLiquidityVanish,
       .delay = {},
       .magnitude = 1'000},
  };

  RiskEngine first(config);
  RiskEngine second(config);
  const auto first_responses =
      run_market_stress_risk_scenario(first, kInstrumentId, kBasePrice, 1, rules, 0);
  const auto second_responses =
      run_market_stress_risk_scenario(second, kInstrumentId, kBasePrice, 1, rules, 0);

  ASSERT_EQ(first_responses.size(), second_responses.size());
  for (std::size_t i = 0; i < first_responses.size(); ++i) {
    EXPECT_EQ(first_responses[i].verdict, second_responses[i].verdict);
    EXPECT_EQ(first_responses[i].reason_code, second_responses[i].reason_code);
  }
}

}  // namespace
