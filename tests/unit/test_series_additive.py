"""M2 slice 7 -- AEGIS-020: difference (additive) back-adjustment.

The convention (ADR-0017's slice 7 addendum): a same-day dual quote at each
roll -- the outgoing contract's own price on the roll date -- gives an
exact gap, which is what makes the series continuous at the splice and the
roll-day return reconcile to the outgoing contract's own realized return
(AEGIS-022; see test_series_reconciliation.py).
"""

from __future__ import annotations

from datetime import date
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from futures.series import (
    InvalidAdjustment,
    PriceObservation,
    build_additive_adjusted_series,
    build_unadjusted_series,
)

pytestmark = pytest.mark.unit

A = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
B = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
D0, D1, D2, D3 = date(2026, 3, 17), date(2026, 3, 18), date(2026, 3, 19), date(2026, 3, 20)


def _known_roll_gap_fixture() -> tuple[list[PriceObservation], dict[date, ContractId]]:
    prices = [
        PriceObservation(A, D0, Decimal(100)),
        PriceObservation(A, D1, Decimal(102)),
        PriceObservation(A, D2, Decimal(103)),  # A's own price on the roll date
        PriceObservation(B, D2, Decimal(150)),  # B becomes front on D2
        PriceObservation(B, D3, Decimal(152)),
    ]
    front_by_date = {D0: A, D1: A, D2: B, D3: B}
    return prices, front_by_date


def test_most_recent_segment_is_unadjusted() -> None:
    prices, front_by_date = _known_roll_gap_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_additive_adjusted_series(unadjusted, prices)
    assert adjusted[-1].adjustment_offset == Decimal(0)
    assert adjusted[-1].adjusted_price == adjusted[-1].raw_price


def test_known_roll_gap_produces_the_documented_offset() -> None:
    """gap = new_price_at_roll (150) - old_price_at_roll (103) = 47; every
    observation strictly before the roll carries that offset."""
    prices, front_by_date = _known_roll_gap_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_additive_adjusted_series(unadjusted, prices)
    assert adjusted[0].adjustment_offset == Decimal(47)
    assert adjusted[1].adjustment_offset == Decimal(47)
    assert adjusted[0].adjusted_price == Decimal(147)
    assert adjusted[1].adjusted_price == Decimal(149)


def test_roll_day_adjusted_return_matches_the_outgoing_contracts_own_return() -> None:
    """The reconciliation property (AEGIS-022): the adjusted return across
    the roll boundary equals A's own realized return that day (103 - 102 =
    1) -- not zero (that would be an invented, "artificial" smoothing) and
    not B's return."""
    prices, front_by_date = _known_roll_gap_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_additive_adjusted_series(unadjusted, prices)
    day_before_roll = adjusted[1]  # D1, last day A is front, raw 102
    roll_day = adjusted[2]  # D2, first day B is front, raw 150
    a_own_return_that_day = Decimal(103) - Decimal(102)  # A's price on D2 minus D1
    assert roll_day.adjusted_price - day_before_roll.adjusted_price == a_own_return_that_day


def test_multiple_sequential_rolls_each_apply_their_own_gap() -> None:
    contract_c = ContractId(venue="SYNX", product_root="EQX", year=2026, month=9)
    d4, d5 = date(2026, 6, 19), date(2026, 6, 20)
    prices = [
        PriceObservation(A, D0, Decimal(100)),
        PriceObservation(A, D1, Decimal(102)),
        PriceObservation(A, D2, Decimal(103)),
        PriceObservation(B, D2, Decimal(150)),
        PriceObservation(B, D3, Decimal(152)),
        PriceObservation(B, d4, Decimal(155)),
        PriceObservation(B, d5, Decimal(156)),  # B's own price on the second roll date
        PriceObservation(contract_c, d5, Decimal(300)),
    ]
    front_by_date = {D0: A, D1: A, D2: B, D3: B, d4: B, d5: contract_c}
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_additive_adjusted_series(unadjusted, prices)

    # Second roll gap: 300 - 156 = 144, applied to every B-segment observation.
    # First roll gap: 150 - 103 = 47, applied on top of that to the A segment.
    by_date = {o.as_of: o for o in adjusted}
    assert by_date[d4].adjustment_offset == Decimal(144)
    assert by_date[D2].adjustment_offset == Decimal(144)
    assert by_date[D1].adjustment_offset == Decimal(144 + 47)
    assert by_date[D0].adjustment_offset == Decimal(144 + 47)
    assert by_date[d5].adjustment_offset == Decimal(0)  # most recent segment


def test_cross_year_roll() -> None:
    d_dec = date(2026, 12, 18)
    d_jan = date(2027, 1, 4)
    prices = [
        PriceObservation(A, d_dec, Decimal(200)),
        PriceObservation(A, d_jan, Decimal(205)),  # A's own price on the roll date
        PriceObservation(B, d_jan, Decimal(210)),
    ]
    front_by_date = {d_dec: A, d_jan: B}
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_additive_adjusted_series(unadjusted, prices)
    assert adjusted[0].adjustment_offset == Decimal(5)  # 210 - 205
    assert adjusted[0].adjusted_price == Decimal(205)
    assert adjusted[1].adjusted_price == Decimal(210)


def test_missing_roll_price_raises_invalid_adjustment() -> None:
    front_by_date = {D0: A, D1: B}
    # No PriceObservation for A on D1 -- the same-day dual quote is absent.
    prices = [PriceObservation(A, D0, Decimal(100)), PriceObservation(B, D1, Decimal(150))]
    unadjusted = build_unadjusted_series(front_by_date, prices)
    with pytest.raises(InvalidAdjustment, match="roll date"):
        build_additive_adjusted_series(unadjusted, prices)


def test_empty_series_returns_empty() -> None:
    assert build_additive_adjusted_series((), []) == ()


def test_single_observation_series_is_its_own_raw_price() -> None:
    front_by_date = {D0: A}
    prices = [PriceObservation(A, D0, Decimal(100))]
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_additive_adjusted_series(unadjusted, prices)
    assert len(adjusted) == 1
    assert adjusted[0].adjusted_price == Decimal(100)
    assert adjusted[0].adjustment_offset == Decimal(0)
