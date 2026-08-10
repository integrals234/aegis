#!/usr/bin/env python3
"""Generate AEGIS-019..022 evidence from the real continuous-series code.

Drives a two-roll, cross-year synthetic price path (the committed EQX
contract identities, M2 slice 2) through the real production functions --
build_unadjusted_series, build_additive_adjusted_series,
build_ratio_adjusted_series, build_return_stream -- and records the
reconciliation checks programmatically, so the evidence proves what
tests/unit/test_series_reconciliation.py proves.

Regenerate with: python3 tools/generate_series_evidence.py
"""

from __future__ import annotations

import json
import subprocess
import sys
from datetime import UTC, date, datetime, timedelta
from decimal import Decimal
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from futures.identifiers import ContractId
from futures.series import (
    PriceObservation,
    build_additive_adjusted_series,
    build_ratio_adjusted_series,
    build_return_stream,
    build_unadjusted_series,
)

A = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
B = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
C = ContractId(venue="SYNX", product_root="EQX", year=2026, month=9)


def _git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else ""


def _provenance() -> dict[str, Any]:
    return {
        "generated_on": datetime.now(UTC).strftime("%Y-%m-%d"),
        "repository_commit": _git("rev-parse", "HEAD"),
        "dirty": bool(_git("status", "--porcelain")),
    }


def _days() -> list[date]:
    # Spans the 2026/2027 year boundary, so the fixture genuinely exercises
    # a cross-year continuous series, not just two same-year rolls.
    base = date(2026, 12, 29)
    return [base + timedelta(days=i) for i in range(6)]


def _additive_fixture() -> tuple[list[PriceObservation], dict[date, ContractId]]:
    d0, d1, d2, d3, d4, d5 = _days()  # d2 = roll A->B (cross-year approach), d4 = roll B->C
    front_by_date = {d0: A, d1: A, d2: B, d3: B, d4: C, d5: C}
    prices = [
        PriceObservation(A, d0, Decimal(100)),
        PriceObservation(A, d1, Decimal(101)),
        PriceObservation(A, d2, Decimal(103)),  # A's own price on roll date d2
        PriceObservation(B, d2, Decimal(150)),
        PriceObservation(B, d3, Decimal(148)),
        PriceObservation(B, d4, Decimal(151)),  # B's own price on roll date d4
        PriceObservation(C, d4, Decimal(300)),
        PriceObservation(C, d5, Decimal(305)),
    ]
    return prices, front_by_date


def _ratio_fixture() -> tuple[list[PriceObservation], dict[date, ContractId]]:
    """Same two-roll, cross-year shape, but with roll-day price pairs
    chosen to divide *exactly* in decimal (1.25, 1.4) -- proportional-return
    reconciliation is only checkable for bit-exact equality when the ratios
    involved terminate (see test_series_reconciliation.py's identical
    reasoning)."""
    d0, d1, d2, d3, d4, d5 = _days()
    front_by_date = {d0: A, d1: A, d2: B, d3: B, d4: C, d5: C}
    prices = [
        PriceObservation(A, d0, Decimal(96)),
        PriceObservation(A, d1, Decimal(98)),
        PriceObservation(A, d2, Decimal(100)),  # A's own price on roll date d2
        PriceObservation(B, d2, Decimal(125)),  # ratio 125/100 = 1.25, exact
        PriceObservation(B, d3, Decimal(128)),
        PriceObservation(B, d4, Decimal(130)),  # B's own price on roll date d4
        PriceObservation(C, d4, Decimal(182)),  # ratio 182/130 = 1.4, exact
        PriceObservation(C, d5, Decimal(190)),
    ]
    return prices, front_by_date


def main(argv: list[str] | None = None) -> int:
    del argv
    prices, front_by_date = _additive_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    additive = build_additive_adjusted_series(unadjusted, prices)

    ratio_prices, ratio_front_by_date = _ratio_fixture()
    ratio_unadjusted = build_unadjusted_series(ratio_front_by_date, ratio_prices)
    ratio = build_ratio_adjusted_series(ratio_unadjusted, ratio_prices)
    returns = build_return_stream(ratio)

    price_lookup = {(p.contract_id, p.session_date): p.price for p in prices}
    ratio_price_lookup = {(p.contract_id, p.session_date): p.price for p in ratio_prices}

    additive_checks = []
    for i, observation in enumerate(unadjusted):
        if i == 0 or not observation.is_roll_point:
            continue
        outgoing = unadjusted[i - 1].contract_id
        roll_day = observation.as_of
        previous_day = unadjusted[i - 1].as_of
        outgoing_delta = price_lookup[(outgoing, roll_day)] - price_lookup[(outgoing, previous_day)]
        additive_delta = additive[i].adjusted_price - additive[i - 1].adjusted_price
        additive_checks.append(
            {
                "roll_date": roll_day.isoformat(),
                "outgoing_contract": outgoing.canonical,
                "incoming_contract": observation.contract_id.canonical,
                "outgoing_contracts_own_price_delta": str(outgoing_delta),
                "additive_adjusted_delta_across_roll": str(additive_delta),
                "reconciles": additive_delta == outgoing_delta,
            }
        )

    ratio_checks = []
    for i, observation in enumerate(ratio_unadjusted):
        if i == 0 or not observation.is_roll_point:
            continue
        outgoing = ratio_unadjusted[i - 1].contract_id
        roll_day = observation.as_of
        previous_day = ratio_unadjusted[i - 1].as_of
        outgoing_return = (
            ratio_price_lookup[(outgoing, roll_day)] / ratio_price_lookup[(outgoing, previous_day)]
        ) - 1
        ratio_return = next(r for r in returns if r.as_of == roll_day).simple_return
        ratio_checks.append(
            {
                "roll_date": roll_day.isoformat(),
                "outgoing_contract": outgoing.canonical,
                "incoming_contract": observation.contract_id.canonical,
                "outgoing_contracts_own_proportional_return": str(outgoing_return),
                "ratio_adjusted_return_across_roll": str(ratio_return),
                "reconciles": ratio_return == outgoing_return,
            }
        )

    if not all(c["reconciles"] for c in additive_checks):
        raise RuntimeError("additive reconciliation check failed; refusing to write evidence with a false claim")
    if not all(c["reconciles"] for c in ratio_checks):
        raise RuntimeError("ratio reconciliation check failed; refusing to write evidence with a false claim")
    if len(additive_checks) != 2 or len(ratio_checks) != 2:
        raise RuntimeError(
            f"expected 2 rolls in each fixture, found {len(additive_checks)} additive, "
            f"{len(ratio_checks)} ratio"
        )

    payload = {
        "artifact": "continuous_series_and_adjustments",
        "requirements": ["AEGIS-019", "AEGIS-020", "AEGIS-021", "AEGIS-022"],
        **_provenance(),
        "additive_unadjusted_series": [
            {
                "as_of": o.as_of.isoformat(),
                "contract_id": o.contract_id.canonical,
                "raw_price": str(o.raw_price),
                "is_roll_point": o.is_roll_point,
            }
            for o in unadjusted
        ],
        "ratio_unadjusted_series": [
            {
                "as_of": o.as_of.isoformat(),
                "contract_id": o.contract_id.canonical,
                "raw_price": str(o.raw_price),
                "is_roll_point": o.is_roll_point,
            }
            for o in ratio_unadjusted
        ],
        "additive_adjusted_series": [
            {
                "as_of": o.as_of.isoformat(),
                "contract_id": o.contract_id.canonical,
                "adjustment_offset": str(o.adjustment_offset),
                "adjusted_price": str(o.adjusted_price),
            }
            for o in additive
        ],
        "ratio_adjusted_series": [
            {
                "as_of": o.as_of.isoformat(),
                "contract_id": o.contract_id.canonical,
                "adjustment_factor": str(o.adjustment_factor),
                "adjusted_price": str(o.adjusted_price),
            }
            for o in ratio
        ],
        "return_stream": [
            {
                "as_of": r.as_of.isoformat(),
                "contract_id": r.contract_id.canonical,
                "is_roll_point": r.is_roll_point,
                "simple_return": str(r.simple_return),
            }
            for r in returns
        ],
        "additive_reconciliation_checks": additive_checks,
        "ratio_reconciliation_checks": ratio_checks,
        "claim": (
            "Two sequential, cross-year rolls (2026-12 into 2027-01, contracts SYNX:EQX:2026H -> "
            "SYNX:EQX:2026M -> SYNX:EQX:2026U) were built through the real production pipeline "
            "(build_unadjusted_series -> build_additive_adjusted_series / "
            "build_ratio_adjusted_series -> build_return_stream). Every observation at every "
            "stage carries its source contract_id (AEGIS-019). For both rolls, the additive "
            "adjusted price delta and the ratio adjusted proportional return across the roll "
            "boundary were checked programmatically against the outgoing contract's own "
            "same-day-dual-quote realized price change and return (AEGIS-020, AEGIS-021, "
            "AEGIS-022) -- verified true for both rolls; this generator raises rather than "
            "writing a claim it cannot support. All data is synthetic "
            "(DATA_AND_RESEARCH_POLICY); no claim is made about any real market."
        ),
    }

    for rid in payload["requirements"]:
        out_dir = ROOT / "experiments/evidence" / rid
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / "continuous_series_and_adjustments.json"
        out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
