#include <gtest/gtest.h>

#include "cpp/participant/portfolio/portfolio_risk.hpp"

/// AEGIS-138: portfolio-level risk analytics. The report is reconciled
/// against the same `Portfolio` state lines every other M5 test/report
/// reads -- this is the C++-side half of that reconciliation;
/// `python/reports` (Batch 2) builds the report-fixture half.
namespace {

using aegis::participant::portfolio::compute_portfolio_risk;
using aegis::participant::portfolio::InstrumentRiskInputs;
using aegis::participant::portfolio::Portfolio;
using aegis::participant::portfolio::StressScenario;

using Side = aegis::events::exchange::Side;

constexpr std::uint32_t kNear = 1;
constexpr std::uint32_t kFar = 2;

TEST(PortfolioRiskAnalytics, GrossNetAndGroupExposureReconcileWithPortfolioState) {
  Portfolio portfolio;
  portfolio.apply_fill(kNear, Side::kSell, 100, 10);  // Short 10 @ 100.
  portfolio.apply_fill(kFar, Side::kBuy, 100, 10);    // Long 10 @ 100.

  const std::unordered_map<std::uint32_t, InstrumentRiskInputs> instruments{
      {kNear,
       InstrumentRiskInputs{
           .multiplier_units = 5, .market = "EQX", .sector = "index", .mark_price_units = 110}},
      {kFar,
       InstrumentRiskInputs{
           .multiplier_units = 5, .market = "EQX", .sector = "index", .mark_price_units = 90}},
  };

  const auto report = compute_portfolio_risk(
      portfolio, instruments, /*equity_units=*/10'000, /*required_margin_units=*/2'000,
      /*volatility_by_instrument=*/{}, /*current_drawdown=*/0.0, /*max_drawdown=*/0.0, {});

  // near: -10 * 110 * 5 = -5'500; far: 10 * 90 * 5 = 4'500.
  EXPECT_EQ(report.gross_exposure_units, 5'500 + 4'500);
  EXPECT_EQ(report.net_exposure_units, -5'500 + 4'500);
  EXPECT_EQ(report.market_exposure_units.at("EQX"), 5'500 + 4'500);
  EXPECT_EQ(report.sector_exposure_units.at("index"), 5'500 + 4'500);
  EXPECT_EQ(report.margin_used_units, 2'000);
  EXPECT_EQ(report.margin_available_units, 8'000);
}

TEST(PortfolioRiskAnalytics, ScriptedStressScenariosAreDeterministicParallelShocks) {
  Portfolio portfolio;
  portfolio.apply_fill(kNear, Side::kBuy, 100, 10);  // Long 10 @ 100.

  const std::unordered_map<std::uint32_t, InstrumentRiskInputs> instruments{
      {kNear,
       InstrumentRiskInputs{
           .multiplier_units = 1, .market = "", .sector = "", .mark_price_units = 100}},
  };
  const std::vector<StressScenario> scenarios{
      StressScenario{.name = "down_10pct", .price_shock_pct = -0.10},
      StressScenario{.name = "up_10pct", .price_shock_pct = 0.10},
  };

  const auto report =
      compute_portfolio_risk(portfolio, instruments, 10'000, 0, {}, 0.0, 0.0, scenarios);
  ASSERT_EQ(report.stress_results.size(), 2U);
  EXPECT_EQ(report.stress_results[0].scenario_name, "down_10pct");
  EXPECT_EQ(report.stress_results[0].pnl_impact_units, -100);  // 10 * (100 * -0.10).
  EXPECT_EQ(report.stress_results[1].scenario_name, "up_10pct");
  EXPECT_EQ(report.stress_results[1].pnl_impact_units, 100);
}

TEST(PortfolioRiskAnalytics, VolatilityAndDrawdownContributionArePassedThroughUnmodified) {
  Portfolio portfolio;
  const std::unordered_map<std::uint32_t, double> volatility{{kNear, 0.05}, {kFar, 0.02}};

  const auto report =
      compute_portfolio_risk(portfolio, {}, 0, 0, volatility, /*current_drawdown=*/12.5,
                             /*max_drawdown=*/40.0, {});
  EXPECT_EQ(report.volatility_contribution.at(kNear), 0.05);
  EXPECT_EQ(report.volatility_contribution.at(kFar), 0.02);
  EXPECT_EQ(report.current_drawdown, 12.5);
  EXPECT_EQ(report.max_drawdown, 40.0);
}

TEST(PortfolioRiskAnalytics, InstrumentsAbsentFromConfigAreExcludedNotFabricated) {
  Portfolio portfolio;
  portfolio.apply_fill(kNear, Side::kBuy, 100,
                       10);  // No InstrumentRiskInputs entry for kNear below.

  const auto report = compute_portfolio_risk(portfolio, /*instruments=*/{}, 0, 0, {}, 0.0, 0.0, {});
  EXPECT_EQ(report.gross_exposure_units, 0);
  EXPECT_EQ(report.net_exposure_units, 0);
  EXPECT_TRUE(report.market_exposure_units.empty());
}

}  // namespace
