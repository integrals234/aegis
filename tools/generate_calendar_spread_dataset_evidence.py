#!/usr/bin/env python3
"""Generate AEGIS-076 evidence: the research dataset reproduces every spread
observation.

AEGIS-076's frozen acceptance is "Research dataset can reproduce every spread
observation." This generator proves that operationally: it builds the
observation sequence twice from the committed inputs alone, checks the two are
identical field for field, and records every observation together with the
provenance needed to rebuild it -- near/far contract identity, both prices,
whether the far price was OBSERVED or CONSTRUCTED, the roll policy that chose
the near leg, and the contract step.

Regenerate with: python3 tools/generate_calendar_spread_dataset_evidence.py
"""

from __future__ import annotations

import json
import sys
from collections.abc import Sequence
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from evidence_provenance import provenance
from research.calendar_spread import CalendarSpreadObservation, build_calendar_spread_observations
from roll_sensitivity_fixture import build_roll_sensitivity_fixture


def _rows(observations: Sequence[CalendarSpreadObservation]) -> list[dict[str, object]]:
    return [
        {
            "as_of": o.as_of.isoformat(),
            "near_contract": o.near_contract_id.canonical,
            "far_contract": o.far_contract_id.canonical,
            "near_price": str(o.near_price),
            "far_price": str(o.far_price),
            "spread": str(o.spread),
            "far_price_observed": o.far_price_observed,
            "far_price_provenance": o.far_price_provenance,
            "roll_policy_name": o.roll_policy_name,
            "contract_steps": o.contract_steps,
        }
        for o in observations
    ]


def main() -> int:
    fixture = build_roll_sensitivity_fixture()
    policy_name = "volume_crossover"

    def build() -> tuple[CalendarSpreadObservation, ...]:
        return build_calendar_spread_observations(
            chain=fixture.chain,
            policy=fixture.policies[policy_name],
            roll_observations=fixture.roll_observations,
            near_prices=fixture.near_prices,
            as_of_dates=fixture.dates,
            basis_rule=fixture.basis_rule,
        )

    first = build()
    second = build()
    reproducible = _rows(first) == _rows(second)
    if not reproducible:
        raise RuntimeError(
            "two builds from identical committed inputs disagree; AEGIS-076's "
            "reproducibility claim cannot be made"
        )

    rows = _rows(first)
    payload = {
        **provenance(),
        "artifact": "calendar_spread_dataset",
        "requirements": ["AEGIS-076"],
        "producer": "tools/generate_calendar_spread_dataset_evidence.py",
        "inputs": {
            "fixture": "tools/roll_sensitivity_fixture.py (synthetic CSX chain, three priced contracts)",
            "roll_policy": policy_name,
        },
        "reproducible_across_rebuilds": reproducible,
        "observation_count": len(rows),
        "observations": rows,
        "far_price_source_counts": {
            "observed": sum(1 for r in rows if r["far_price_observed"]),
            "constructed": sum(1 for r in rows if not r["far_price_observed"]),
        },
        "claim": (
            "AEGIS-076: the real research.calendar_spread."
            "build_calendar_spread_observations was run twice over identical "
            "committed inputs and produced identical observations, each carrying the "
            "provenance needed to reproduce it: near and far contract identity, both "
            "prices, whether the far price was observed or constructed (and from "
            "what), the roll policy that selected the near leg, and the contract step."
        ),
        "not_evidence_for": [
            "any claim about real markets -- this fixture's prices are synthetic "
            "throughout (ADR-0025)",
            "AEGIS-024's sensitivity result, which is a separate artifact",
            "execution quality -- no order, fill or venue is modelled here",
        ],
    }

    out_dir = ROOT / "experiments/evidence/AEGIS-076"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "calendar_spread_dataset.json"
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
