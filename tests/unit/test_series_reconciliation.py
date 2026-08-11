"""M2 slice 7 -- AEGIS-022: return reconciliation across every roll.

The property both adjustment conventions satisfy (ADR-0017's slice 7
addendum, derived and proven in test_series_additive.py/test_series_ratio.py
for a single roll each): on a non-roll day, the adjusted return equals the
raw same-contract return exactly; on a roll day, it equals the outgoing
contract's own realized return for that day. This file proves the
reconciliation holds across *every* roll in a multi-roll series at once, via
`build_return_stream`, rather than one hand-picked day at a time.

No synthetic index is built here -- the M2 plan of record marks it optional,
and no frozen acceptance criterion for AEGIS-019..022 names one.
"""

from __future__ import annotations

import itertools
from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from futures.series import (
    InvalidAdjustment,
    PriceObservation,
    build_additive_adjusted_series,
    build_ratio_adjusted_series,
    build_return_stream,
    build_unadjusted_series,
)

pytestmark = pytest.mark.unit

A = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
B = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
C = ContractId(venue="SYNX", product_root="EQX", year=2026, month=9)


def _multi_roll_fixture() -> tuple[list[PriceObservation], dict[date, ContractId]]:
    """Two sequential rolls, cross-year, each with its own same-day dual
    quote and its own non-trivial front-contract price path."""
    base = date(2026, 12, 1)
    days = [base + timedelta(days=i) for i in range(6)]
    d0, d1, d2, d3, d4, d5 = days  # d2 = first roll (A->B), d4 = second roll (B->C)

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


def _multi_roll_ratio_fixture() -> tuple[list[PriceObservation], dict[date, ContractId]]:
    """Same two-roll, cross-year shape as :func:`_multi_roll_fixture`, but
    with every roll-day price pair chosen to divide *exactly* in decimal
    (1.25 and 1.4) -- proportional-return reconciliation is only checkable
    for bit-exact equality when the ratios involved terminate; a fixture
    like 150/103 is correct but not exactly representable at any fixed
    `Decimal` precision, which would make an exact-equality assertion a
    precision artifact rather than a genuine check.
    """
    base = date(2026, 12, 1)
    days = [base + timedelta(days=i) for i in range(6)]
    d0, d1, d2, d3, d4, d5 = days  # d2 = first roll (A->B), d4 = second roll (B->C)

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


def test_ratio_reconciliation_holds_on_every_non_roll_day() -> None:
    prices, front_by_date = _multi_roll_ratio_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_ratio_adjusted_series(unadjusted, prices)
    returns = build_return_stream(adjusted)

    raw_by_date = {o.as_of: o.raw_price for o in unadjusted}
    dates = sorted(raw_by_date)
    date_pairs = list(itertools.pairwise(dates))
    for (previous_date, current_date), ret in zip(date_pairs, returns, strict=True):
        if not ret.is_roll_point:
            expected = (raw_by_date[current_date] / raw_by_date[previous_date]) - 1
            assert ret.simple_return == expected


def test_ratio_reconciliation_holds_on_every_roll_day() -> None:
    """On a roll day, the adjusted return equals the OUTGOING contract's own
    realized return that day -- computed independently here from the raw
    same-day dual quote, not from the adjusted series itself."""
    prices, front_by_date = _multi_roll_ratio_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_ratio_adjusted_series(unadjusted, prices)
    returns = build_return_stream(adjusted)

    price_lookup = {(p.contract_id, p.session_date): p.price for p in prices}
    roll_days = [r for r in returns if r.is_roll_point]
    assert len(roll_days) == 2  # both rolls in the fixture

    for i, observation in enumerate(unadjusted):
        if not observation.is_roll_point or i == 0:
            continue
        outgoing_contract = unadjusted[i - 1].contract_id
        roll_date = observation.as_of
        previous_date = unadjusted[i - 1].as_of
        outgoing_return = (
            price_lookup[(outgoing_contract, roll_date)] / price_lookup[(outgoing_contract, previous_date)]
        ) - 1
        matching = next(r for r in returns if r.as_of == roll_date)
        assert matching.simple_return == outgoing_return


def test_additive_reconciliation_holds_on_every_non_roll_day() -> None:
    prices, front_by_date = _multi_roll_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_additive_adjusted_series(unadjusted, prices)

    for previous, current in itertools.pairwise(adjusted):
        if not current.is_roll_point:
            expected_delta = current.raw_price - previous.raw_price
            assert current.adjusted_price - previous.adjusted_price == expected_delta


def test_additive_reconciliation_holds_on_every_roll_day() -> None:
    prices, front_by_date = _multi_roll_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_additive_adjusted_series(unadjusted, prices)
    price_lookup = {(p.contract_id, p.session_date): p.price for p in prices}

    for i, observation in enumerate(adjusted):
        if not observation.is_roll_point or i == 0:
            continue
        outgoing_contract = unadjusted[i - 1].contract_id
        roll_date = observation.as_of
        previous_date = unadjusted[i - 1].as_of
        outgoing_delta = (
            price_lookup[(outgoing_contract, roll_date)] - price_lookup[(outgoing_contract, previous_date)]
        )
        assert observation.adjusted_price - adjusted[i - 1].adjusted_price == outgoing_delta


def test_provenance_survives_through_every_stage() -> None:
    """AEGIS-019's provenance requirement, checked end to end: unadjusted,
    additive, ratio and the return stream all name the correct contract for
    every observation."""
    prices, front_by_date = _multi_roll_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    additive = build_additive_adjusted_series(unadjusted, prices)
    ratio = build_ratio_adjusted_series(unadjusted, prices)
    returns = build_return_stream(ratio)

    expected = [front_by_date[d] for d in sorted(front_by_date)]
    assert [o.contract_id for o in unadjusted] == expected
    assert [o.contract_id for o in additive] == expected
    assert [o.contract_id for o in ratio] == expected
    assert [r.contract_id for r in returns] == expected[1:]


def test_return_stream_rejects_zero_adjusted_price() -> None:
    class _Point:
        def __init__(self, as_of: date, contract_id: ContractId, adjusted_price: Decimal) -> None:
            self.as_of = as_of
            self.contract_id = contract_id
            self.adjusted_price = adjusted_price
            self.is_roll_point = False

    points = [_Point(date(2026, 1, 1), A, Decimal(0)), _Point(date(2026, 1, 2), A, Decimal(100))]
    with pytest.raises(InvalidAdjustment, match="zero"):
        build_return_stream(points)  # type: ignore[arg-type]


def test_return_stream_on_fewer_than_two_points_is_empty() -> None:
    prices, _front_by_date = _multi_roll_fixture()
    unadjusted = build_unadjusted_series({date(2026, 12, 1): A}, prices)
    ratio = build_ratio_adjusted_series(unadjusted, prices)
    assert build_return_stream(ratio) == ()
