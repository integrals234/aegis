"""M2 slice 7 -- AEGIS-019: unadjusted continuous series.

Built from explicit roll selections (a caller-supplied front-contract-per-
date mapping), not by calling a roll policy internally -- selection
(M2 slice 6) and consumption (this slice) are separate concerns.
"""

from __future__ import annotations

from datetime import date
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from futures.series import MissingPrice, PriceObservation, build_unadjusted_series

pytestmark = pytest.mark.unit

A = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
B = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
D0, D1, D2, D3 = date(2026, 3, 17), date(2026, 3, 18), date(2026, 3, 19), date(2026, 3, 20)


def test_every_observation_carries_contract_provenance() -> None:
    front_by_date = {D0: A, D1: B}
    prices = [PriceObservation(A, D0, Decimal(100)), PriceObservation(B, D1, Decimal(150))]
    series = build_unadjusted_series(front_by_date, prices)
    assert [o.contract_id for o in series] == [A, B]


def test_raw_prices_are_preserved_exactly() -> None:
    front_by_date = {D0: A}
    prices = [PriceObservation(A, D0, Decimal("100.25"))]
    series = build_unadjusted_series(front_by_date, prices)
    assert series[0].raw_price == Decimal("100.25")


def test_is_roll_point_marks_exactly_the_contract_change() -> None:
    front_by_date = {D0: A, D1: A, D2: B, D3: B}
    prices = [
        PriceObservation(A, D0, Decimal(100)),
        PriceObservation(A, D1, Decimal(101)),
        PriceObservation(B, D2, Decimal(150)),
        PriceObservation(B, D3, Decimal(151)),
    ]
    series = build_unadjusted_series(front_by_date, prices)
    assert [o.is_roll_point for o in series] == [False, False, True, False]


def test_first_observation_is_never_a_roll_point() -> None:
    front_by_date = {D0: A}
    prices = [PriceObservation(A, D0, Decimal(100))]
    series = build_unadjusted_series(front_by_date, prices)
    assert series[0].is_roll_point is False


def test_output_is_sorted_by_date_regardless_of_mapping_order() -> None:
    front_by_date = {D2: B, D0: A, D1: A}
    prices = [
        PriceObservation(A, D0, Decimal(100)),
        PriceObservation(A, D1, Decimal(101)),
        PriceObservation(B, D2, Decimal(150)),
    ]
    series = build_unadjusted_series(front_by_date, prices)
    assert [o.as_of for o in series] == [D0, D1, D2]


def test_missing_price_for_selected_front_raises() -> None:
    front_by_date = {D0: A}
    with pytest.raises(MissingPrice):
        build_unadjusted_series(front_by_date, [])


def test_missing_price_names_the_contract_and_date() -> None:
    front_by_date = {D0: A}
    with pytest.raises(MissingPrice, match=r"SYNX:EQX:2026H.*2026-03-17"):
        build_unadjusted_series(front_by_date, [])


def test_price_for_a_non_front_contract_is_not_used() -> None:
    """A price observation for a contract that was never selected as front
    on a given date must not leak into the series for that date."""
    front_by_date = {D0: A}
    prices = [PriceObservation(B, D0, Decimal(999))]  # wrong contract for D0
    with pytest.raises(MissingPrice):
        build_unadjusted_series(front_by_date, prices)


def test_empty_selection_yields_empty_series() -> None:
    assert build_unadjusted_series({}, []) == ()
