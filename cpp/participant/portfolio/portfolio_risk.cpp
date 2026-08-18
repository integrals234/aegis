#include "cpp/participant/portfolio/portfolio_risk.hpp"

#include <cmath>

namespace aegis::participant::portfolio {
namespace {
[[nodiscard]] constexpr std::int64_t abs64(std::int64_t value) { return value < 0 ? -value : value; }
}  // namespace

PortfolioRiskReport compute_portfolio_risk(
    const Portfolio& portfolio, const std::unordered_map<std::uint32_t, InstrumentRiskInputs>& instruments,
    std::int64_t equity_units, std::int64_t required_margin_units,
    const std::unordered_map<std::uint32_t, double>& volatility_by_instrument, double current_drawdown,
    double max_drawdown, const std::vector<StressScenario>& scenarios) {
  PortfolioRiskReport report;
  report.margin_used_units = required_margin_units;
  report.margin_available_units = equity_units - required_margin_units;
  report.volatility_contribution = volatility_by_instrument;
  report.current_drawdown = current_drawdown;
  report.max_drawdown = max_drawdown;

  for (const auto& [instrument_id, position] : portfolio.all_positions()) {
    const auto found = instruments.find(instrument_id);
    if (found == instruments.end() || position.quantity_units == 0) {
      continue;
    }
    const InstrumentRiskInputs& info = found->second;
    const std::int64_t signed_exposure =
        position.quantity_units * info.mark_price_units * info.multiplier_units;
    report.gross_exposure_units += abs64(signed_exposure);
    report.net_exposure_units += signed_exposure;
    if (!info.market.empty()) {
      report.market_exposure_units[info.market] += abs64(signed_exposure);
    }
    if (!info.sector.empty()) {
      report.sector_exposure_units[info.sector] += abs64(signed_exposure);
    }
  }

  for (const StressScenario& scenario : scenarios) {
    std::int64_t pnl_impact = 0;
    for (const auto& [instrument_id, position] : portfolio.all_positions()) {
      const auto found = instruments.find(instrument_id);
      if (found == instruments.end() || position.quantity_units == 0) {
        continue;
      }
      const InstrumentRiskInputs& info = found->second;
      const double shocked_price_delta =
          static_cast<double>(info.mark_price_units) * scenario.price_shock_pct;
      pnl_impact += static_cast<std::int64_t>(static_cast<double>(position.quantity_units) *
                                              shocked_price_delta *
                                              static_cast<double>(info.multiplier_units));
    }
    report.stress_results.push_back(StressResult{.scenario_name = scenario.name, .pnl_impact_units = pnl_impact});
  }

  return report;
}

}  // namespace aegis::participant::portfolio
