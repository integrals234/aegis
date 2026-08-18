#!/usr/bin/env python3
"""Generate AEGIS-079 evidence: the Dickey-Fuller stationarity test result
over a real calendar-spread observation sequence.

Runs the actual `research.stationarity.test_spread_stationarity` over the
same `research.calendar_spread`-built observation sequence
`tests/unit/test_stationarity.py`'s fixtures exercise the underlying test
function against directly -- this generator does not re-derive the test, it
calls the real production code path once and records the result.

Regenerate with: python3 tools/generate_stationarity_evidence.py
"""

from __future__ import annotations

import json
import sys
from datetime import date
from decimal import Decimal
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from evidence_provenance import provenance
from research.calendar_spread import build_calendar_spread_observations
from research.stationarity import test_spread_stationarity
from roll_sensitivity_fixture import build_roll_sensitivity_fixture


def _json_default(value: Any) -> Any:
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, date):
        return value.isoformat()
    raise TypeError(f"not JSON serializable: {type(value).__name__}")


def main() -> int:
    fixture = build_roll_sensitivity_fixture()
    policy_name = "volume_crossover"
    observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=fixture.policies[policy_name],
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )
    result = test_spread_stationarity(observations)

    payload = {
        **provenance(),
        "artifact": "stationarity_test",
        "requirements": ["AEGIS-079"],
        "dataset_id": "CSX-synthetic-multi-contract (tools/roll_sensitivity_fixture.py)",
        "roll_policy_name": policy_name,
        "spread_series": [str(o.spread) for o in observations],
        "test_result": {
            "test_name": result.test_name,
            "null_hypothesis": result.null_hypothesis,
            "alternative_hypothesis": result.alternative_hypothesis,
            "sample_size": result.sample_size,
            "regression_intercept": str(result.regression_intercept),
            "regression_slope": str(result.regression_slope),
            "test_statistic": str(result.test_statistic),
            "significance_level": result.significance_level,
            "critical_value": str(result.critical_value),
            "classification": result.classification.value,
            "assumptions": list(result.assumptions),
            "caveats": list(result.caveats),
        },
        "claim": (
            "AEGIS-079: the real research.stationarity.test_spread_stationarity was run "
            "once, over a real research.calendar_spread observation sequence, and produced "
            f"the test_result recorded above (classification: {result.classification.value})."
        ),
        "not_evidence_for": [
            "any claim that the spread WILL remain stationary in the future -- see "
            "test_result.caveats",
            "any claim about real markets -- the underlying near/far price data is "
            "synthetic/constructed (ADR-0025); see tools/roll_sensitivity_fixture.py",
            "a claim that this is the only valid stationarity test -- it is the one, "
            "documented test this module implements (ADR-0026), not a comparison across "
            "several",
        ],
    }

    out_dir = ROOT / "experiments/evidence/AEGIS-079"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "stationarity_test.json"
    out_path.write_text(
        json.dumps(payload, indent=2, sort_keys=True, default=_json_default) + "\n", encoding="utf-8"
    )
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
