#!/usr/bin/env python3
"""Generate AEGIS-015..018 roll golden evidence from the real policy code.

Drives the real, committed EQX contract chain (M2 slice 2) through each of
the four production roll policies with fixed, documented parameters and
observation fixtures, recording exactly which contract each policy selects
on which date -- proving what the golden-fixture tests prove, not a
parallel claim.

Regenerate with: python3 tools/generate_roll_evidence.py
"""

from __future__ import annotations

import json
import subprocess
import sys
from datetime import date, timedelta
from decimal import Decimal
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from futures.chain import ContractChain
from futures.roll.fixed_days import FixedDaysPolicy
from futures.roll.liquidity_score import LiquidityScorePolicy
from futures.roll.oi_crossover import OpenInterestCrossoverPolicy
from futures.roll.policy import RollObservation, listed_contract_ids_at
from futures.roll.volume_crossover import VolumeCrossoverPolicy
from make_futures_fixtures import load_family


def _git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else ""


def _provenance() -> dict[str, Any]:
    from datetime import UTC, datetime

    return {
        "generated_on": datetime.now(UTC).strftime("%Y-%m-%d"),
        "repository_commit": _git("rev-parse", "HEAD"),
        "dirty": bool(_git("status", "--porcelain")),
    }


def _eqx_chain() -> ContractChain:
    venue, product_root, contracts = load_family(ROOT / "data_samples/futures/eqx.json")
    chain = ContractChain(venue, product_root)
    for contract in contracts:
        chain.add(contract)
    return chain


def generate_aegis_015() -> dict[str, Any]:
    chain = _eqx_chain()
    policy = FixedDaysPolicy(days_before_expiry=5)
    dates = [date(2026, 3, d) for d in (10, 13, 14, 15, 16, 19, 20)]
    rolls = [
        {"as_of": d.isoformat(), "front_contract": (fc := policy.front_contract(chain, [], d))
         and fc.canonical}
        for d in dates
    ]
    return {
        "artifact": "fixed_days_golden",
        "requirement": "AEGIS-015",
        **_provenance(),
        "policy_parameters": {"days_before_expiry": policy.days_before_expiry},
        "chain": "data_samples/futures/eqx.json",
        "rolls": rolls,
        "claim": (
            "FixedDaysPolicy(days_before_expiry=5) run against the committed EQX contract "
            "chain rolls from SYNX:EQX:2026H to SYNX:EQX:2026M exactly at the inclusive "
            "5-calendar-day boundary (2026-03-15), staying on the front contract through "
            "2026-03-14 and remaining rolled thereafter -- computed by the real production "
            "policy, not asserted."
        ),
    }


def generate_aegis_016() -> dict[str, Any]:
    chain = _eqx_chain()
    listed = listed_contract_ids_at(chain, date(2026, 1, 10))
    front_id, deferred_id = listed[0], listed[1]
    base = date(2026, 1, 1)
    observations = []
    for i in range(10):
        d = base + timedelta(days=i)
        observations.append(RollObservation(front_id, d, 1000 - i * 80, None))
        observations.append(RollObservation(deferred_id, d, 200 + i * 90, None))

    policy = VolumeCrossoverPolicy(persistence_days=2)
    rolls = [
        {
            "as_of": (d := base + timedelta(days=i)).isoformat(),
            "front_contract": (fc := policy.front_contract(chain, observations, d)) and fc.canonical,
            "front_volume": 1000 - i * 80,
            "deferred_volume": 200 + i * 90,
        }
        for i in range(10)
    ]
    return {
        "artifact": "volume_crossover_golden",
        "requirement": "AEGIS-016",
        **_provenance(),
        "policy_parameters": {"persistence_days": policy.persistence_days},
        "chain": "data_samples/futures/eqx.json",
        "front_contract_id": front_id.canonical,
        "deferred_contract_id": deferred_id.canonical,
        "rolls": rolls,
        "claim": (
            "VolumeCrossoverPolicy(persistence_days=2) against a declining-front/rising-"
            "deferred synthetic volume series rolls on 2026-01-07: the deferred contract's "
            "volume first exceeds the front's on 2026-01-06 (one day, not yet persisted), and "
            "having exceeded it again on 2026-01-07 (the second consecutive comparable day), "
            "the persistence-2 threshold is met and the roll occurs that day -- computed by "
            "the real production policy."
        ),
    }


def generate_aegis_017() -> dict[str, Any]:
    chain = _eqx_chain()
    listed = listed_contract_ids_at(chain, date(2026, 1, 10))
    front_id, deferred_id = listed[0], listed[1]
    base = date(2026, 1, 1)
    observations = []
    for i in range(8):
        d = base + timedelta(days=i)
        observations.append(RollObservation(front_id, d, None, 5000 - i * 300))
        observations.append(RollObservation(deferred_id, d, None, 1000 + i * 700))

    policy = OpenInterestCrossoverPolicy(persistence_days=2)
    rolls = [
        {
            "as_of": (d := base + timedelta(days=i)).isoformat(),
            "front_contract": (fc := policy.front_contract(chain, observations, d)) and fc.canonical,
            "front_open_interest": 5000 - i * 300,
            "deferred_open_interest": 1000 + i * 700,
        }
        for i in range(8)
    ]
    return {
        "artifact": "oi_crossover_golden",
        "requirement": "AEGIS-017",
        **_provenance(),
        "policy_parameters": {"persistence_days": policy.persistence_days},
        "chain": "data_samples/futures/eqx.json",
        "front_contract_id": front_id.canonical,
        "deferred_contract_id": deferred_id.canonical,
        "rolls": rolls,
        "claim": (
            "OpenInterestCrossoverPolicy(persistence_days=2) against a declining-front/"
            "rising-deferred synthetic open-interest series rolls at the second consecutive "
            "day of crossover -- computed by the real production policy."
        ),
    }


def generate_aegis_018() -> dict[str, Any]:
    chain = _eqx_chain()
    listed = listed_contract_ids_at(chain, date(2026, 1, 10))
    front_id, deferred_id = listed[0], listed[1]
    as_of = date(2026, 1, 10)
    observations = [
        RollObservation(front_id, as_of, volume=1000, open_interest=2000),
        RollObservation(deferred_id, as_of, volume=3000, open_interest=1000),
    ]
    policy = LiquidityScorePolicy(volume_weight=Decimal("0.7"), open_interest_weight=Decimal("0.3"))
    breakdown = policy.score_breakdown(listed, observations, as_of)
    front = policy.front_contract(chain, observations, as_of)

    return {
        "artifact": "liquidity_score_golden",
        "requirement": "AEGIS-018",
        **_provenance(),
        "policy_parameters": {
            "volume_weight": str(policy.volume_weight),
            "open_interest_weight": str(policy.open_interest_weight),
        },
        "chain": "data_samples/futures/eqx.json",
        "as_of": as_of.isoformat(),
        "selected_front_contract": front.canonical if front else None,
        "breakdown": [
            {
                "contract_id": entry.contract_id.canonical,
                "volume": entry.volume,
                "open_interest": entry.open_interest,
                "volume_component": str(entry.volume_component),
                "open_interest_component": str(entry.open_interest_component),
                "score": str(entry.score),
            }
            for entry in breakdown
        ],
        "claim": (
            "LiquidityScorePolicy(volume_weight=0.7, open_interest_weight=0.3) against a "
            "synthetic two-contract observation selects the higher-combined-score contract, "
            "with every component of the score (normalized volume share, normalized "
            "open-interest share, weighted sum) exposed and exactly reproducible via Decimal "
            "arithmetic -- computed by the real production policy."
        ),
    }


def main(argv: list[str] | None = None) -> int:
    del argv
    generators = {
        "AEGIS-015": ("fixed_days_golden", generate_aegis_015),
        "AEGIS-016": ("volume_crossover_golden", generate_aegis_016),
        "AEGIS-017": ("oi_crossover_golden", generate_aegis_017),
        "AEGIS-018": ("liquidity_score_golden", generate_aegis_018),
    }
    for rid, (name, generator) in generators.items():
        payload = generator()
        out_dir = ROOT / "experiments/evidence" / rid
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / f"{name}.json"
        out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
