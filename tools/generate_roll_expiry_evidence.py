#!/usr/bin/env python3
"""Generate AEGIS-081 evidence: before/on/after-roll calendar-spread slicing
against a real, deterministic roll transition.

Runs the actual `research.roll_expiry_effects.compute_roll_expiry_effects`
over `tools/roll_sensitivity_fixture.py`'s `volume_crossover` policy, the
same fixture whose one real roll transition
`tests/unit/test_roll_expiry_effects.py` slices against.

Regenerate with: python3 tools/generate_roll_expiry_evidence.py
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
from research.roll_expiry_effects import compute_roll_expiry_effects
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
    policy = fixture.policies[policy_name]
    observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=policy,
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )
    result = compute_roll_expiry_effects(
        chain=fixture.chain,
        policy=policy,
        roll_observations=fixture.roll_observations,
        prices=fixture.near_prices,
        dates=fixture.dates,
        observations=observations,
        replay_config=fixture.replay_config,
    )

    payload = {
        **provenance(),
        "artifact": "roll_expiry_effects",
        "requirements": ["AEGIS-081"],
        "dataset_id": "CSX-synthetic-multi-contract (tools/roll_sensitivity_fixture.py)",
        "roll_policy_name": result.roll_policy_name,
        "roll_dates": [d.isoformat() for d in result.roll_dates],
        "slices": [
            {
                "slice": m.slice.value,
                "observation_count": m.observation_count,
                "mean_spread": str(m.mean_spread) if m.mean_spread is not None else None,
                "mean_expiry_distance_days": m.mean_expiry_distance_days,
                "entry_count": m.entry_count,
                "exit_count": m.exit_count,
            }
            for m in result.slices
        ],
        "claim": (
            "AEGIS-081: the real research.roll_expiry_effects.compute_roll_expiry_effects "
            "was run once over a real deterministic roll transition (real "
            "futures.roll_audit.build_roll_audit output, unmodified) and produced the "
            "before/on/after-roll slice metrics recorded above."
        ),
        "not_evidence_for": [
            "any claim about real markets -- the underlying near/far price data is "
            "synthetic/constructed (ADR-0025)",
            "the general cross-strategy attribution framework reserved for M6 -- this is "
            "the narrower, M4-specific roll/expiry analysis only",
        ],
    }

    out_dir = ROOT / "experiments/evidence/AEGIS-081"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "roll_expiry_effects.json"
    out_path.write_text(
        json.dumps(payload, indent=2, sort_keys=True, default=_json_default) + "\n", encoding="utf-8"
    )
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
