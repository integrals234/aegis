#!/usr/bin/env python3
"""Generate the committed synthetic futures contract fixtures (AEGIS-011, AEGIS-012).

Mirrors ``tools/make_sample_data.py``'s reasoning: the fixtures are generated
from fixed rules rather than sliced from any real feed, so the redistributable
thing is one that contains no market data at all (DATA_AND_RESEARCH_POLICY).
Unlike the book-event sample, there is no randomness to seed — contract dates
are arithmetic on fixed offsets, not sampled — so "deterministic" here means
"re-running this file reproduces the committed output byte for byte", not
"seeded".

Three product families (AEGIS-011's fixture-coverage floor), each with three
contracts spanning a year boundary (enough to exercise chain ordering and
lookup without inflating fixture size):

* ``EQX`` — quarterly, cash-settled. ``last_trade_date == expiry``, which
  exercises the lifecycle boundary where ``LAST_TRADING_DAY`` takes precedence
  over ``SETTLED`` on the same day.
* ``CLX`` — monthly, physically settled. ``last_trade_date`` precedes
  ``expiry`` by a few days, which exercises the ``EXPIRED`` window actually
  having non-zero width.
* ``SRX`` — quarterly, cash-settled, non-contiguous months, to prove chain
  ordering does not depend on the contracts being adjacent or on generation
  order.

Expiry falls on a fixed day-of-month (the 20th) for every contract. Real
expiry calendars follow business-day and holiday rules; that is AEGIS-013 (M2
slice 3) and does not exist yet, so a fixed day avoids smuggling calendar logic
into a contract-identity fixture.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from datetime import date, timedelta
from pathlib import Path
from typing import Final

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId

FUTURES_DIR: Final[str] = "data_samples/futures"
EXPIRY_DAY: Final[int] = 20


@dataclass(frozen=True)
class FamilySpec:
    venue: str
    product_root: str
    settlement_type: SettlementType
    months: tuple[tuple[int, int], ...]  # (year, month) pairs, in generation order
    last_trade_offset_days: int  # 0 => last_trade_date == expiry
    first_trade_offset_days: int  # days before expiry that trading begins


FAMILIES: Final[tuple[FamilySpec, ...]] = (
    FamilySpec(
        venue="SYNX",
        product_root="EQX",
        settlement_type=SettlementType.CASH,
        months=((2026, 3), (2026, 6), (2027, 3)),
        last_trade_offset_days=0,
        first_trade_offset_days=365,
    ),
    FamilySpec(
        venue="SYNX",
        product_root="CLX",
        settlement_type=SettlementType.PHYSICAL,
        months=((2026, 1), (2026, 3), (2026, 7)),
        last_trade_offset_days=3,
        first_trade_offset_days=300,
    ),
    FamilySpec(
        venue="SYNX",
        product_root="SRX",
        settlement_type=SettlementType.CASH,
        months=((2026, 6), (2026, 12), (2027, 6)),
        last_trade_offset_days=0,
        first_trade_offset_days=500,
    ),
)


def build_contract(spec: FamilySpec, year: int, month: int) -> Contract:
    contract_id = ContractId(venue=spec.venue, product_root=spec.product_root, year=year, month=month)
    expiry = date(year, month, EXPIRY_DAY)
    last_trade_date = expiry - timedelta(days=spec.last_trade_offset_days)
    first_trade_date = expiry - timedelta(days=spec.first_trade_offset_days)
    return Contract(
        contract_id=contract_id,
        first_trade_date=first_trade_date,
        last_trade_date=last_trade_date,
        expiry=expiry,
        settlement_type=spec.settlement_type,
    )


def build_family(spec: FamilySpec) -> dict[str, object]:
    contracts = [build_contract(spec, year, month) for year, month in spec.months]
    return {
        "venue": spec.venue,
        "product_root": spec.product_root,
        "contracts": [
            {
                "contract_id": contract.canonical,
                "first_trade_date": contract.first_trade_date.isoformat(),
                "last_trade_date": contract.last_trade_date.isoformat(),
                "expiry": contract.expiry.isoformat(),
                "settlement_type": contract.settlement_type.value,
            }
            for contract in contracts
        ],
    }


def load_family(path: Path) -> tuple[str, str, list[Contract]]:
    """Read a fixture back into ``Contract`` objects.

    The inverse of :func:`build_family`, and the only parser: tests and the
    AEGIS-012 evidence generator both call this rather than each carrying a
    parallel JSON-to-``Contract`` translation that could drift out of step.
    """
    document = json.loads(path.read_text(encoding="utf-8"))
    contracts = [
        Contract(
            contract_id=ContractId.parse(entry["contract_id"]),
            first_trade_date=date.fromisoformat(entry["first_trade_date"]),
            last_trade_date=date.fromisoformat(entry["last_trade_date"]),
            expiry=date.fromisoformat(entry["expiry"]),
            settlement_type=SettlementType(entry["settlement_type"]),
        )
        for entry in document["contracts"]
    ]
    return document["venue"], document["product_root"], contracts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / FUTURES_DIR)
    args = parser.parse_args(argv)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    for spec in FAMILIES:
        family = build_family(spec)
        output_path = args.output_dir / f"{spec.product_root.lower()}.json"
        output_path.write_text(json.dumps(family, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {output_path} ({output_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
