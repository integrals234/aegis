#!/usr/bin/env python3
"""Render and commit the three M4 Batch 2 reports (AEGIS-079, AEGIS-081,
AEGIS-024), on the shared deterministic report foundation
(`python/reports/report_model.py`, Batch 1).

Each report is built from the same real research computation the
corresponding evidence generator (`tools/generate_stationarity_evidence.py`,
`tools/generate_roll_expiry_evidence.py`,
`tools/generate_roll_sensitivity_strategy_evidence.py`) already proves --
this script does not recompute anything differently, only renders the
report_model form of the same result.

Regenerate with: python3 tools/generate_batch2_reports.py
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from reports.roll_expiry_report import build_roll_expiry_report
from reports.roll_sensitivity_report import build_roll_sensitivity_report
from reports.stationarity_report import build_stationarity_report
from research.calendar_spread import build_calendar_spread_observations
from research.roll_expiry_effects import compute_roll_expiry_effects
from research.roll_method_sensitivity import compute_roll_method_strategy_sensitivity
from research.stationarity import test_spread_stationarity
from roll_sensitivity_fixture import build_roll_sensitivity_fixture

INPUT_PATHS = ["data_samples/futures/bars/eqx.csv"]
DATASET_ID = "CSX-synthetic-multi-contract (tools/roll_sensitivity_fixture.py)"


def main() -> int:
    fixture = build_roll_sensitivity_fixture()

    # AEGIS-079.
    stationarity_policy_name = "volume_crossover"
    stationarity_observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=fixture.policies[stationarity_policy_name],
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )
    stationarity_result = test_spread_stationarity(stationarity_observations)
    stationarity_report = build_stationarity_report(
        ROOT, INPUT_PATHS, DATASET_ID, stationarity_policy_name, stationarity_result
    )
    _write(ROOT / "experiments/evidence/AEGIS-079/stationarity_report.json", stationarity_report)

    # AEGIS-081.
    roll_expiry_result = compute_roll_expiry_effects(
        chain=fixture.chain,
        policy=fixture.policies[stationarity_policy_name],
        roll_observations=fixture.roll_observations,
        prices=fixture.near_prices,
        dates=fixture.dates,
        observations=stationarity_observations,
        replay_config=fixture.replay_config,
    )
    roll_expiry_report = build_roll_expiry_report(ROOT, INPUT_PATHS, DATASET_ID, roll_expiry_result)
    _write(ROOT / "experiments/evidence/AEGIS-081/roll_expiry_report.json", roll_expiry_report)

    # AEGIS-024.
    sensitivity_result = compute_roll_method_strategy_sensitivity(
        fixture.chain,
        fixture.policies,
        fixture.roll_observations,
        fixture.near_prices,
        fixture.dates,
        fixture.basis_rule,
        fixture.replay_config,
    )
    sensitivity_report = build_roll_sensitivity_report(ROOT, INPUT_PATHS, DATASET_ID, sensitivity_result)
    _write(
        ROOT / "experiments/evidence/AEGIS-024/roll_method_strategy_sensitivity_report.json",
        sensitivity_report,
    )

    return 0


def _write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    print(f"wrote {path.relative_to(ROOT)} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    raise SystemExit(main())
