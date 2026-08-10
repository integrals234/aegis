"""M2 slice 7 -- AEGIS-021: ratio (multiplicative) back-adjustment.

Same same-day-dual-quote convention as AEGIS-020 (test_series_additive.py),
multiplicatively: proportional returns are preserved across a roll rather
than absolute price changes.
"""

from __future__ import annotations

from datetime import date
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from futures.series import (
    InvalidAdjustment,
    PriceObservation,
    build_ratio_adjusted_series,
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
        PriceObservation(B, D2, Decimal(150)),
        PriceObservation(B, D3, Decimal(152)),
    ]
    front_by_date = {D0: A, D1: A, D2: B, D3: B}
    return prices, front_by_date


def test_most_recent_segment_has_factor_one() -> None:
    prices, front_by_date = _known_roll_gap_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_ratio_adjusted_series(unadjusted, prices)
    assert adjusted[-1].adjustment_factor == Decimal(1)
    assert adjusted[-1].adjusted_price == adjusted[-1].raw_price


def test_known_roll_gap_produces_the_documented_ratio() -> None:
    prices, front_by_date = _known_roll_gap_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_ratio_adjusted_series(unadjusted, prices)
    expected_ratio = Decimal(150) / Decimal(103)
    assert adjusted[0].adjustment_factor == expected_ratio
    assert adjusted[1].adjustment_factor == expected_ratio
    assert adjusted[0].adjusted_price == Decimal(100) * expected_ratio


def test_roll_day_proportional_return_matches_outgoing_contracts_own_return() -> None:
    """Reconciliation (AEGIS-022): the ratio-adjusted proportional return
    across the roll equals A's own proportional return that day."""
    prices, front_by_date = _known_roll_gap_fixture()
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_ratio_adjusted_series(unadjusted, prices)
    day_before_roll, roll_day = adjusted[1], adjusted[2]
    proportional_return = (roll_day.adjusted_price / day_before_roll.adjusted_price) - 1
    a_own_proportional_return = (Decimal(103) / Decimal(102)) - 1
    assert proportional_return == a_own_proportional_return


def test_multiple_sequential_rolls_compound_their_ratios() -> None:
    contract_c = ContractId(venue="SYNX", product_root="EQX", year=2026, month=9)
    d4, d5 = date(2026, 6, 19), date(2026, 6, 20)
    prices = [
        PriceObservation(A, D0, Decimal(100)),
        PriceObservation(A, D1, Decimal(102)),
        PriceObservation(A, D2, Decimal(103)),
        PriceObservation(B, D2, Decimal(150)),
        PriceObservation(B, D3, Decimal(152)),
        PriceObservation(B, d4, Decimal(155)),
        PriceObservation(B, d5, Decimal(156)),
        PriceObservation(contract_c, d5, Decimal(300)),
    ]
    front_by_date = {D0: A, D1: A, D2: B, D3: B, d4: B, d5: contract_c}
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_ratio_adjusted_series(unadjusted, prices)
    by_date = {o.as_of: o for o in adjusted}

    ratio2 = Decimal(300) / Decimal(156)
    ratio1 = Decimal(150) / Decimal(103)
    assert by_date[d5].adjustment_factor == Decimal(1)
    assert by_date[d4].adjustment_factor == ratio2
    assert by_date[D2].adjustment_factor == ratio2
    assert by_date[D1].adjustment_factor == ratio2 * ratio1
    assert by_date[D0].adjustment_factor == ratio2 * ratio1


def test_cross_year_roll() -> None:
    d_dec = date(2026, 12, 18)
    d_jan = date(2027, 1, 4)
    prices = [
        PriceObservation(A, d_dec, Decimal(200)),
        PriceObservation(A, d_jan, Decimal(205)),
        PriceObservation(B, d_jan, Decimal(210)),
    ]
    front_by_date = {d_dec: A, d_jan: B}
    unadjusted = build_unadjusted_series(front_by_date, prices)
    adjusted = build_ratio_adjusted_series(unadjusted, prices)
    expected_ratio = Decimal(210) / Decimal(205)
    assert adjusted[0].adjustment_factor == expected_ratio


def test_missing_roll_price_raises_invalid_adjustment() -> None:
    front_by_date = {D0: A, D1: B}
    prices = [PriceObservation(A, D0, Decimal(100)), PriceObservation(B, D1, Decimal(150))]
    unadjusted = build_unadjusted_series(front_by_date, prices)
    with pytest.raises(InvalidAdjustment, match="roll date"):
        build_ratio_adjusted_series(unadjusted, prices)


def test_zero_roll_price_raises_invalid_adjustment_not_zero_division() -> None:
    front_by_date = {D0: A, D1: B}
    prices = [
        PriceObservation(A, D0, Decimal(100)),
        PriceObservation(A, D1, Decimal(0)),  # zero price on the roll date
        PriceObservation(B, D1, Decimal(150)),
    ]
    unadjusted = build_unadjusted_series(front_by_date, prices)
    with pytest.raises(InvalidAdjustment, match="non-positive"):
        build_ratio_adjusted_series(unadjusted, prices)


def test_negative_roll_price_raises_invalid_adjustment() -> None:
    front_by_date = {D0: A, D1: B}
    prices = [
        PriceObservation(A, D0, Decimal(100)),
        PriceObservation(A, D1, Decimal(-5)),
        PriceObservation(B, D1, Decimal(150)),
    ]
    unadjusted = build_unadjusted_series(front_by_date, prices)
    with pytest.raises(InvalidAdjustment, match="non-positive"):
        build_ratio_adjusted_series(unadjusted, prices)


def test_empty_series_returns_empty() -> None:
    assert build_ratio_adjusted_series((), []) == ()
