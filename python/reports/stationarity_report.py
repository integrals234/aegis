"""AEGIS-079 stationarity report: renders a
:class:`~research.stationarity.StationarityTestResult` through the shared M4
report foundation (`python/reports/report_model.py`). No new report
machinery -- this is one thin content module on top of Batch 1's foundation.
"""

from __future__ import annotations

from collections.abc import Sequence
from pathlib import Path

from research.stationarity import StationarityTestResult

from reports.report_model import build_report_provenance, render_report

__all__ = ["build_stationarity_report"]


def build_stationarity_report(
    root: Path,
    input_paths: Sequence[str],
    dataset_id: str,
    roll_policy_name: str,
    result: StationarityTestResult,
) -> str:
    provenance = build_report_provenance(
        report_id="AEGIS-079-stationarity",
        root=root,
        input_paths=input_paths,
        strategy_config={},  # No strategy configuration: this report concerns the spread series alone.
        dataset_id=dataset_id,
        roll_policy_name=roll_policy_name,
    )
    findings = {
        "test_name": result.test_name,
        "null_hypothesis": result.null_hypothesis,
        "alternative_hypothesis": result.alternative_hypothesis,
        "sample_size": result.sample_size,
        "regression_intercept": result.regression_intercept,
        "regression_slope": result.regression_slope,
        "test_statistic": result.test_statistic,
        "significance_level": result.significance_level,
        "critical_value": result.critical_value,
        "classification": result.classification.value,
        "assumptions": list(result.assumptions),
        "caveats": list(result.caveats),
        "data_disclosure": (
            "the spread series is derived from data_samples/futures bars plus a "
            "documented constructed far-leg basis (ADR-0025); it is not observed "
            "tick data, and this report makes no claim about real markets or future "
            "behaviour."
        ),
    }
    return render_report(provenance, findings)
