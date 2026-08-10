#!/usr/bin/env python3
"""Generate AEGIS-023/024 evidence from the real roll-audit code (M2 slice 8).

AEGIS-023's acceptance is "machine-readable and human-readable reports match
fixtures", so the artifact records *both* renderings of the same audit and the
generator refuses to write them unless they describe the same rolls -- one data
source, two views, checked rather than asserted.

AEGIS-024's acceptance names an experiment report quantifying *strategy*
differences caused by roll choices. No strategy exists before M4, so what is
recorded here is exactly the M2-owned half: the roll-date and adjusted-price-
path divergence between policies, produced by the real `compare_roll_methods`.
The artifact states that boundary in its own `not_evidence_for` field so it
cannot be mistaken for the M4 residual being paid.

Regenerate with: python3 tools/generate_roll_audit_evidence.py
"""

from __future__ import annotations

import json
import sys
from datetime import date, timedelta
from decimal import Decimal
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from evidence_provenance import provenance
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.roll.fixed_days import FixedDaysPolicy
from futures.roll.oi_crossover import OpenInterestCrossoverPolicy
from futures.roll.policy import RollObservation
from futures.roll.volume_crossover import VolumeCrossoverPolicy
from futures.roll_audit import build_roll_audit, render_human_readable, to_machine_readable
from futures.roll_sensitivity import compare_roll_methods
from futures.series import PriceObservation

FRONT = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
DEFERRED = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
BASE_DAY = date(2026, 1, 1)


def _chain() -> ContractChain:
    chain = ContractChain("SYNX", "EQX")
    for contract_id, expiry in ((FRONT, date(2026, 3, 20)), (DEFERRED, date(2026, 6, 20))):
        chain.add(
            Contract(
                contract_id=contract_id,
                first_trade_date=date(2025, 1, 1),
                last_trade_date=expiry,
                expiry=expiry,
                settlement_type=SettlementType.CASH,
            )
        )
    return chain


def _scenario() -> tuple[list[RollObservation], list[PriceObservation], list[date]]:
    """The same crossover scenario tests/unit/test_roll_audit.py pins, so the
    evidence and the test describe one behaviour rather than two."""
    dates = [BASE_DAY + timedelta(days=i) for i in range(10)]
    observations: list[RollObservation] = []
    prices: list[PriceObservation] = []
    for index, day in enumerate(dates):
        # Front volume decays, deferred volume grows: they cross, and the
        # crossover persists, which is what triggers the roll.
        observations.append(RollObservation(FRONT, day, 1000 - index * 80, 5000 - index * 300))
        observations.append(RollObservation(DEFERRED, day, 200 + index * 90, 1000 + index * 400))
        prices.append(PriceObservation(FRONT, day, Decimal(100 + index)))
        prices.append(PriceObservation(DEFERRED, day, Decimal(150 + index)))
    return observations, prices, dates


def main(argv: list[str] | None = None) -> int:
    del argv
    chain = _chain()
    observations, prices, dates = _scenario()

    policy = VolumeCrossoverPolicy(persistence_days=2)
    audit = build_roll_audit(chain, policy, observations, prices, dates)
    machine = to_machine_readable(audit)
    human = render_human_readable(audit)

    # AEGIS-023: the two renderings must describe the same rolls. A header line
    # plus one row per record; every record's own fields must appear in its row.
    human_rows = human.strip().splitlines()
    if len(human_rows) != len(machine) + 1:
        raise RuntimeError(
            f"human-readable report has {len(human_rows) - 1} rows but the "
            f"machine-readable report has {len(machine)}; refusing to write a false claim"
        )
    for record, row in zip(machine, human_rows[1:], strict=True):
        for field in ("as_of", "old_contract", "new_contract", "raw_gap", "trigger"):
            if str(record[field]) not in row:
                raise RuntimeError(
                    f"machine field {field}={record[field]!r} is absent from its "
                    f"human-readable row {row!r}; the two views have diverged"
                )
    if not machine:
        raise RuntimeError("the scenario produced no roll; the evidence would prove nothing")

    # AEGIS-024 (M2-owned half): policy-vs-policy divergence, real production code.
    comparisons = compare_roll_methods(
        chain,
        {
            "fixed_days_10": FixedDaysPolicy(days_before_expiry=10),
            "volume_crossover_2d": VolumeCrossoverPolicy(persistence_days=2),
            "oi_crossover_2d": OpenInterestCrossoverPolicy(persistence_days=2),
        },
        observations,
        prices,
        dates,
    )
    if not any(c.roll_dates_differ for c in comparisons):
        raise RuntimeError(
            "no pair of policies chose different roll dates; this scenario cannot "
            "demonstrate roll-method sensitivity and the evidence would be vacuous"
        )

    payload: dict[str, Any] = {
        "artifact": "roll_audit_and_sensitivity",
        "requirements": ["AEGIS-023", "AEGIS-024"],
        **provenance(),
        "scenario": {
            "chain": [FRONT.canonical, DEFERRED.canonical],
            "sessions": [day.isoformat() for day in dates],
            "description": (
                "Front-month volume and open interest decay while the deferred "
                "contract's grow, so the two cross and the crossover persists. "
                "All data is synthetic (DATA_AND_RESEARCH_POLICY)."
            ),
        },
        "machine_readable_report": machine,
        "human_readable_report": human,
        "roll_method_comparisons": [
            {
                "policy_a": c.policy_a,
                "policy_b": c.policy_b,
                "roll_dates_a": [d.isoformat() for d in c.roll_dates_a],
                "roll_dates_b": [d.isoformat() for d in c.roll_dates_b],
                "roll_dates_differ": c.roll_dates_differ,
                "max_abs_price_deviation": str(c.max_abs_price_deviation),
                "mean_abs_price_deviation": str(c.mean_abs_price_deviation),
            }
            for c in comparisons
        ],
        "claim": (
            "AEGIS-023: one roll audit was produced by the real build_roll_audit over "
            "the committed EQX chain and a synthetic volume-crossover scenario, and "
            "rendered both machine-readably and human-readably from the same records. "
            "This generator checks row-for-row that the two renderings describe the same "
            "rolls and raises rather than writing a claim it cannot support. "
            "AEGIS-024 (M2-owned half): three roll policies were compared pairwise by the "
            "real compare_roll_methods, recording the roll dates each chose and the "
            "maximum/mean absolute deviation between their additive-adjusted price paths; "
            "at least one pair genuinely diverges, or this generator raises."
        ),
        "not_evidence_for": [
            "AEGIS-024's full acceptance: quantifying STRATEGY differences needs a "
            "strategy, which configs/architecture_rules.yaml dates to M4. The M4 "
            "residual is registered in docs/DEFERRED_VERIFICATION.md and is NOT "
            "discharged by this artifact.",
            "any claim about real markets: every price, volume and open-interest "
            "figure here is synthetic.",
        ],
    }

    for rid in payload["requirements"]:
        out_dir = ROOT / "experiments/evidence" / rid
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / "roll_audit_and_sensitivity.json"
        out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
