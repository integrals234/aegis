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
import csv
import io
import json
import sys
from dataclasses import dataclass
from datetime import UTC, date, datetime, time, timedelta
from decimal import Decimal
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


BAR_DIR: Final[str] = "data_samples/futures/bars"

# AEGIS-026 -- one small, deterministic bar fixture per family, proving all
# three load through the single python/futures/ingest.py interface. Each
# reuses that family's *first* FAMILIES contract (already committed by the
# section above) so there is exactly one contract-identity source of truth.
# Two input formats are exercised (CSV, JSON Lines), per the M2 plan of
# record's ingestion scope.


@dataclass(frozen=True)
class BarFamilySpec:
    product_root: str
    tick_size: Decimal
    base_price: Decimal
    start_date: date
    bar_count: int
    file_format: str  # "csv" | "jsonl"


BAR_FAMILIES: Final[tuple[BarFamilySpec, ...]] = (
    BarFamilySpec("EQX", Decimal("0.25"), Decimal("5000.00"), date(2026, 3, 10), 6, "csv"),
    BarFamilySpec("CLX", Decimal("0.01"), Decimal("70.00"), date(2026, 1, 5), 6, "jsonl"),
    BarFamilySpec("SRX", Decimal("0.005"), Decimal("98.500"), date(2026, 6, 8), 6, "csv"),
)

BAR_COLUMNS: Final[tuple[str, ...]] = (
    "contract_symbol",
    "event_time_ns",
    "open",
    "high",
    "low",
    "close",
    "settlement_price",
    "volume",
    "open_interest",
    "source_sequence",
)


def _noon_utc_ns(day: date) -> int:
    delta = datetime.combine(day, time(12, 0), tzinfo=UTC) - datetime(1970, 1, 1, tzinfo=UTC)
    return (delta.days * 86400 + delta.seconds) * 1_000_000_000


def build_bars(spec: BarFamilySpec, contract: Contract) -> list[dict[str, str]]:
    """Deterministic, tick-aligned synthetic bars. No randomness: each bar's
    prices are the previous bar's close plus a fixed number of ticks, so
    re-running this file reproduces the committed output byte for byte."""
    bars: list[dict[str, str]] = []
    price = spec.base_price
    for i in range(spec.bar_count):
        event_day = spec.start_date + timedelta(days=i)
        open_p = price
        high_p = price + spec.tick_size * 4
        low_p = price - spec.tick_size * 2
        close_p = price + spec.tick_size
        bars.append(
            {
                "contract_symbol": contract.canonical,
                "event_time_ns": str(_noon_utc_ns(event_day)),
                "open": str(open_p),
                "high": str(high_p),
                "low": str(low_p),
                "close": str(close_p),
                "settlement_price": str(close_p),
                "volume": str(1_000 + i * 10),
                "open_interest": str(5_000 + i * 5),
                "source_sequence": str(i),
            }
        )
        price = close_p
    return bars


def write_bar_csv(path: Path, bars: list[dict[str, str]]) -> None:
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=list(BAR_COLUMNS), lineterminator="\n")
    writer.writeheader()
    for bar in bars:
        writer.writerow(bar)
    path.write_text(buffer.getvalue(), encoding="utf-8")


_JSON_INT_FIELDS: Final[tuple[str, ...]] = ("event_time_ns", "volume", "open_interest", "source_sequence")


def write_bar_jsonl(path: Path, bars: list[dict[str, str]]) -> None:
    """Unlike the CSV writer, JSON has native integers -- so the JSONL fixture
    also proves ingest.py's native-int branch, not only its digit-string
    branch (CSV has no types, so that file always exercises the latter)."""
    records: list[dict[str, object]] = [
        {**bar, **{field: int(bar[field]) for field in _JSON_INT_FIELDS}} for bar in bars
    ]
    lines = [json.dumps(record, sort_keys=True, separators=(",", ":")) for record in records]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_bar_fixtures(output_dir: Path) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    family_by_root = {spec.product_root: spec for spec in FAMILIES}
    for bar_spec in BAR_FAMILIES:
        family_spec = family_by_root[bar_spec.product_root]
        first_year, first_month = family_spec.months[0]
        contract = build_contract(family_spec, first_year, first_month)
        bars = build_bars(bar_spec, contract)
        suffix = "csv" if bar_spec.file_format == "csv" else "jsonl"
        out_path = output_dir / f"{bar_spec.product_root.lower()}.{suffix}"
        if bar_spec.file_format == "csv":
            write_bar_csv(out_path, bars)
        else:
            write_bar_jsonl(out_path, bars)
        written.append(out_path)
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / FUTURES_DIR)
    parser.add_argument("--bars-output-dir", type=Path, default=ROOT / BAR_DIR)
    args = parser.parse_args(argv)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    for spec in FAMILIES:
        family = build_family(spec)
        output_path = args.output_dir / f"{spec.product_root.lower()}.json"
        output_path.write_text(json.dumps(family, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {output_path} ({output_path.stat().st_size} bytes)")

    for bar_path in write_bar_fixtures(args.bars_output_dir):
        print(f"wrote {bar_path} ({bar_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
