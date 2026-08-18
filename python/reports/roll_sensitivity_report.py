"""AEGIS-024 roll-method strategy sensitivity report: renders a
:class:`~research.roll_method_sensitivity.RollMethodStrategySensitivityResult`
through the shared M4 report foundation."""

from __future__ import annotations

from collections.abc import Sequence
from pathlib import Path

from research.roll_method_sensitivity import RollMethodStrategySensitivityResult

from reports.report_model import build_report_provenance, render_report

__all__ = ["build_roll_sensitivity_report"]


def build_roll_sensitivity_report(
    root: Path, input_paths: Sequence[str], dataset_id: str, result: RollMethodStrategySensitivityResult
) -> str:
    policy_names = sorted(m.policy_name for m in result.strategy_metrics_by_policy)
    provenance = build_report_provenance(
        report_id="AEGIS-024-roll-method-strategy-sensitivity",
        root=root,
        input_paths=input_paths,
        strategy_config={"compared_policies": policy_names},
        dataset_id=dataset_id,
        roll_policy_name=",".join(policy_names),
    )
    findings = {
        "price_path_comparisons": [
            {
                "policy_a": c.policy_a,
                "policy_b": c.policy_b,
                "roll_dates_a": [d.isoformat() for d in c.roll_dates_a],
                "roll_dates_b": [d.isoformat() for d in c.roll_dates_b],
                "roll_dates_differ": c.roll_dates_differ,
                "max_abs_price_deviation": c.max_abs_price_deviation,
                "mean_abs_price_deviation": c.mean_abs_price_deviation,
            }
            for c in result.price_path_comparisons
        ],
        "strategy_metrics_by_policy": [
            {
                "policy_name": m.policy_name,
                "signal_count": m.signal_count,
                "entry_count": m.entry_count,
                "exit_count": m.exit_count,
                "round_trip_count": m.round_trip_count,
                "total_realized_pnl": m.total_realized_pnl,
                "final_position": m.final_position.value,
            }
            for m in result.strategy_metrics_by_policy
        ],
        "interpretation_limits": (
            "this report quantifies SENSITIVITY: how the same strategy's signals and "
            "P&L differ across roll policies over one disclosed-synthetic dataset "
            "(ADR-0025). It does not claim any policy is universally better, and makes "
            "no claim about real markets or future performance."
        ),
    }
    return render_report(provenance, findings)
