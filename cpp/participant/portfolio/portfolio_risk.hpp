#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/participant/portfolio/portfolio.hpp"

/// AEGIS-138: portfolio-level risk analytics.
///
/// `cpp-participant-portfolio.may_depend_on = [cpp-common, cpp-events]` only
/// (`configs/architecture_rules.yaml`) -- no edge to `cpp-participant-risk`.
/// This module therefore takes every risk-owned figure (equity, required
/// margin, per-instrument realized volatility, drawdown) as plain data,
/// supplied by the composition root, which is the only layer permitted to
/// see both `risk::RiskEngine` and `Portfolio` at once (ADR-0027).
namespace aegis::participant::portfolio {

struct InstrumentRiskInputs {
  std::int64_t multiplier_units{1};
  std::string market;
  std::string sector;
  std::int64_t mark_price_units{0};
};

/// A deterministic, scripted "what if" shock -- never a statistical
/// simulation -- applied as a uniform parallel shift to every position's own
/// mark price. `volatility_multiple`/`liquidity_factor` are reported
/// alongside the P&L impact as context for a reader, not separately applied
/// (docs/LIMITATIONS.md: this is a scenario disclosure, not a claim of
/// liquidity-adjusted execution modelling).
struct StressScenario {
  std::string name;
  double price_shock_pct{0.0};
  double volatility_multiple{1.0};
  double liquidity_factor{1.0};
};

struct StressResult {
  std::string scenario_name;
  std::int64_t pnl_impact_units{0};
};

struct PortfolioRiskReport {
  std::int64_t gross_exposure_units{0};
  std::int64_t net_exposure_units{0};
  std::int64_t margin_used_units{0};
  std::int64_t margin_available_units{0};
  std::unordered_map<std::string, std::int64_t> market_exposure_units;
  std::unordered_map<std::string, std::int64_t> sector_exposure_units;
  std::unordered_map<std::uint32_t, double> volatility_contribution;
  double current_drawdown{0.0};
  double max_drawdown{0.0};
  std::vector<StressResult> stress_results;
};

/// Every instrument absent from `instruments` (no configured mark/multiplier)
/// contributes nothing to gross/net/group exposure -- excluded, not
/// fabricated, matching `RiskEngine::gross_portfolio_notional_units`'s same
/// convention on the risk side.
[[nodiscard]] PortfolioRiskReport compute_portfolio_risk(
    const Portfolio& portfolio, const std::unordered_map<std::uint32_t, InstrumentRiskInputs>& instruments,
    std::int64_t equity_units, std::int64_t required_margin_units,
    const std::unordered_map<std::uint32_t, double>& volatility_by_instrument, double current_drawdown,
    double max_drawdown, const std::vector<StressScenario>& scenarios);

}  // namespace aegis::participant::portfolio
