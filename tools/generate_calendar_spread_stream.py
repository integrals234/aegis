#!/usr/bin/env python3
"""Generate the committed M4 calendar-spread demo fixture (ADR-0025).

Builds ``tests/unit/fixtures/participant/calendar_spread_stream.jsonl`` from
the real committed EQX contracts/bars in ``data_samples/futures`` through the
production `python/research` code path -- this script does not re-derive
roll or spread logic, it calls `research.calendar_spread` and
`research.stream_builder` exactly as the test suite does.

The near leg's price is real (EQX settlement bars). The far leg's price is
**not** observed -- `data_samples` carries only one contract's bar history
per product -- and is constructed by a fixed, documented per-observation
basis sequence (`_BASIS_UNITS_BY_INDEX`, in dollars) chosen so the resulting
spread series genuinely varies, which is what lets the demo strategy cross
its entry/exit thresholds. See ADR-0025.

Regenerate with:  python3 tools/generate_calendar_spread_stream.py
"""

from __future__ import annotations

import csv
import datetime
import json
import sys
from datetime import date
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from futures.chain import ContractChain
from futures.roll.fixed_days import FixedDaysPolicy
from futures.series import PriceObservation
from make_futures_fixtures import load_family
from research.calendar_spread import ConstructedBasisRule, build_calendar_spread_observations
from research.stream_builder import StreamBuildConfig, build_two_leg_stream_records

FIXTURE_PATH = ROOT / "data_samples/futures/eqx.json"
BARS_PATH = ROOT / "data_samples/futures/bars/eqx.csv"
OUTPUT_PATH = ROOT / "tests/unit/fixtures/participant/calendar_spread_stream.jsonl"

# One basis value per observation (0-based index, wrapping): the far leg's
# constructed price is near_price + this value. Chosen -- not fitted -- so
# the resulting spread series (which equals this sequence exactly, since the
# rule is purely additive) crosses a default CalendarSpreadStrategy's entry
# (|z| >= 2.0) and exit (|z| <= 0.5) thresholds at least once each, over a
# RollingZScore window scored against the *prior* observations only. See
# tests/cpp/unit/test_calendar_spread_strategy.cpp for the exact z-score
# arithmetic this produces.
_BASIS_UNITS_BY_INDEX = (
    Decimal("0.50"),
    Decimal("0.55"),
    Decimal("0.60"),
    Decimal("0.65"),
    Decimal("2.50"),
    Decimal("0.70"),
)
_BASIS_DESCRIPTION = (
    "constructed: far_price = near_price + basis[index % 6], "
    "basis in dollars = [0.50, 0.55, 0.60, 0.65, 2.50, 0.70]; "
    "NOT observed market data (ADR-0025)"
)


def main() -> int:
    venue, product_root, contracts = load_family(FIXTURE_PATH)
    chain = ContractChain(venue, product_root)
    for contract in contracts:
        chain.add(contract)

    near_prices: list[PriceObservation] = []
    as_of_dates = []
    with BARS_PATH.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            contract_id = next(
                c.contract_id for c in contracts if c.contract_id.canonical == row["contract_symbol"]
            )
            session_date = _ns_to_date(int(row["event_time_ns"]))
            near_prices.append(
                PriceObservation(
                    contract_id=contract_id,
                    session_date=session_date,
                    price=Decimal(row["settlement_price"]),
                )
            )
            as_of_dates.append(session_date)

    policy = FixedDaysPolicy(days_before_expiry=0)
    basis_rule = ConstructedBasisRule(
        basis_units_by_index=_BASIS_UNITS_BY_INDEX, description=_BASIS_DESCRIPTION
    )
    observations = build_calendar_spread_observations(
        chain=chain,
        policy=policy,
        roll_observations=(),
        near_prices=near_prices,
        as_of_dates=as_of_dates,
        basis_rule=basis_rule,
    )

    records = build_two_leg_stream_records(
        observations,
        StreamBuildConfig(
            near_instrument_id=2001,
            far_instrument_id=2002,
            tick_spread_units=10,
            quote_quantity_units=100,
        ),
    )

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT_PATH.open("w", encoding="utf-8") as handle:
        for record in records:
            handle.write(json.dumps(record, sort_keys=True, separators=(",", ":")))
            handle.write("\n")

    print(f"wrote {len(records)} records ({len(observations)} dates) to {OUTPUT_PATH}")
    return 0


def _ns_to_date(event_time_ns: int) -> date:
    return datetime.datetime.fromtimestamp(event_time_ns / 1e9, tz=datetime.UTC).date()


if __name__ == "__main__":
    raise SystemExit(main())
