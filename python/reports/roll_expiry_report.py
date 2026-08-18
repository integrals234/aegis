"""AEGIS-081 roll/expiry effects report: renders a
:class:`~research.roll_expiry_effects.RollExpiryEffectsResult` through the
shared M4 report foundation."""

from __future__ import annotations

from collections.abc import Sequence
from pathlib import Path

from research.roll_expiry_effects import RollExpiryEffectsResult

from reports.report_model import build_report_provenance, render_report

__all__ = ["build_roll_expiry_report"]


def build_roll_expiry_report(
    root: Path, input_paths: Sequence[str], dataset_id: str, result: RollExpiryEffectsResult
) -> str:
    provenance = build_report_provenance(
        report_id="AEGIS-081-roll-expiry-effects",
        root=root,
        input_paths=input_paths,
        strategy_config={},
        dataset_id=dataset_id,
        roll_policy_name=result.roll_policy_name,
    )
    findings = {
        "roll_dates": [d.isoformat() for d in result.roll_dates],
        "slices": [
            {
                "slice": m.slice.value,
                "observation_count": m.observation_count,
                "mean_spread": m.mean_spread,
                "mean_expiry_distance_days": m.mean_expiry_distance_days,
                "entry_count": m.entry_count,
                "exit_count": m.exit_count,
            }
            for m in result.slices
        ],
        "interpretation_limits": (
            "slice metrics describe this single disclosed-synthetic dataset and roll "
            "policy only (ADR-0025); a slice with zero observations reports null "
            "metrics rather than an invented value, and no claim is made about roll "
            "effects in real markets."
        ),
    }
    return render_report(provenance, findings)
