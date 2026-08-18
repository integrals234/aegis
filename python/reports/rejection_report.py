"""M5 formal strategy-rejection report (AEGIS-155): renders a
:class:`~validation.rejection.RejectionReport` through the shared report
foundation. The verdict and every evaluated criterion (triggered or not)
are recorded verbatim -- this module computes nothing itself.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from validation.rejection import RejectionReport

from reports.report_model import build_report_provenance, render_report

__all__ = ["build_rejection_report"]


def build_rejection_report(
    root: Path,
    input_paths: Sequence[str],
    dataset_id: str,
    roll_policy_name: str,
    strategy_name: str,
    report: RejectionReport,
    *,
    strategy_config: Mapping[str, Any],
) -> str:
    provenance = build_report_provenance(
        report_id="AEGIS-155-rejection",
        root=root,
        input_paths=input_paths,
        strategy_config=dict(strategy_config),
        dataset_id=dataset_id,
        roll_policy_name=roll_policy_name,
    )
    findings = {
        "strategy_name": strategy_name,
        "verdict": report.as_record()["verdict"],
        "criteria": report.as_record()["criteria"],
        "triggering_criteria": [c.name for c in report.triggering_criteria],
        "data_disclosure": (
            "This verdict is computed over synthetic/in-repo data only and does not "
            "establish live profitability, production risk adequacy, or generalization "
            "to real markets (docs/DATA_AND_RESEARCH_POLICY.md)."
        ),
    }
    return render_report(provenance, findings)
