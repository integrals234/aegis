"""M5 validation report: renders the anti-overfitting validation suite's
results (AEGIS-139..154) through the shared report foundation
(`python/reports/report_model.py`). One thin content module, like every
other report here -- no new report machinery.

Every field this report carries is a live-computed result object; nothing
here recomputes or approximates a validation module's own output.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from validation.leakage import LeakageAuditResult
from validation.markets import MultiMarketReport
from validation.regimes import RegimeReport
from validation.resampling import BootstrapResult, MonteCarloResult
from validation.roll_sensitivity import PolicyPnlDifference
from validation.sensitivity import CostSweepResult, FillSweepResult, LatencySweepResult
from validation.stability import StabilitySurface

from reports.report_model import build_report_provenance, render_report

__all__ = ["build_validation_report"]

_DATA_HONESTY_DISCLOSURE = (
    "All prices are synthetic/in-repo (SYNX is not a real venue); quotes are "
    "constructed daily series, not observed tick data. Matching-engine logic "
    "elsewhere in this platform is real; the market data validated here is "
    "not. Risk controls exercised elsewhere are software controls under "
    "simulation. Fill assumptions here are validation models, not observed "
    "fills. Nothing in this report establishes live profitability or "
    "production risk adequacy (docs/DATA_AND_RESEARCH_POLICY.md, ADR-0029)."
)


def build_validation_report(
    root: Path,
    input_paths: Sequence[str],
    dataset_id: str,
    roll_policy_name: str,
    *,
    strategy_config: Mapping[str, Any],
    stability: StabilitySurface | None = None,
    cost_sensitivity: CostSweepResult | None = None,
    latency_sensitivity: LatencySweepResult | None = None,
    fill_sensitivity: FillSweepResult | None = None,
    bootstrap: BootstrapResult | None = None,
    monte_carlo: MonteCarloResult | None = None,
    multi_market: MultiMarketReport | None = None,
    regimes: RegimeReport | None = None,
    roll_differences: tuple[PolicyPnlDifference, ...] | None = None,
    leakage_audit: LeakageAuditResult | None = None,
) -> str:
    provenance = build_report_provenance(
        report_id="AEGIS-139-155-validation",
        root=root,
        input_paths=input_paths,
        strategy_config=dict(strategy_config),
        dataset_id=dataset_id,
        roll_policy_name=roll_policy_name,
    )

    findings: dict[str, Any] = {"data_disclosure": _DATA_HONESTY_DISCLOSURE}

    if stability is not None:
        findings["parameter_stability"] = {
            "points": stability.as_records(),
            "best": {
                "zscore_window": stability.best.zscore_window,
                "entry_threshold": stability.best.entry_threshold,
                "exit_threshold": stability.best.exit_threshold,
                "total_pnl": str(stability.best.total_pnl),
            },
            "metric_mean": stability.metric_mean,
            "metric_stdev": stability.metric_stdev,
        }
    if cost_sensitivity is not None:
        findings["transaction_cost_sensitivity"] = {
            "points": cost_sensitivity.as_records(),
            "break_even_index": cost_sensitivity.break_even_index,
        }
    if latency_sensitivity is not None:
        findings["latency_sensitivity"] = {"points": latency_sensitivity.as_records()}
    if fill_sensitivity is not None:
        findings["slippage_and_fill_sensitivity"] = {"points": fill_sensitivity.as_records()}
    if bootstrap is not None:
        findings["bootstrap_confidence_interval"] = {
            "statistic_name": bootstrap.statistic_name,
            "sample_unit": bootstrap.sample_unit,
            "resampling_method": bootstrap.resampling_method,
            "num_draws": bootstrap.num_draws,
            "confidence_level": bootstrap.confidence_level,
            "seed": bootstrap.seed,
            "point_estimate": bootstrap.point_estimate,
            "lower": bootstrap.lower,
            "upper": bootstrap.upper,
            "assumptions": bootstrap.assumptions,
            "limitations": bootstrap.limitations,
        }
    if monte_carlo is not None:
        findings["monte_carlo_path_resampling"] = {
            "num_paths": monte_carlo.num_paths,
            "seed": monte_carlo.seed,
            "trade_count": monte_carlo.trade_count,
            "ending_pnl_quantiles": monte_carlo.ending_pnl_quantiles,
            "max_drawdown_quantiles": monte_carlo.max_drawdown_quantiles,
        }
    if multi_market is not None:
        findings["multi_market"] = multi_market.as_records()
    if regimes is not None:
        findings["regimes"] = regimes.as_records()
    if roll_differences is not None:
        findings["roll_method_differences"] = [
            {"policy_a": d.policy_a, "policy_b": d.policy_b, "total_pnl_difference": str(d.total_pnl_difference)}
            for d in roll_differences
        ]
    if leakage_audit is not None:
        findings["leakage_audit"] = {
            "record_count": leakage_audit.record_count,
            "passed": leakage_audit.passed,
            "violations": leakage_audit.as_records(),
        }

    return render_report(provenance, findings)
