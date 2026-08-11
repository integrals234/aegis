"""M2 slice 7 -- invariants across arbitrary valid roll schedules, not just
the golden fixtures in tests/unit/test_series_*.py.

* Deterministic output: identical input produces an identical series, every
  time.
* No dependence on insertion/hash order: shuffling the `prices` list must
  not change the result.
* Provenance always present: every observation, at every stage (unadjusted,
  additive, ratio, return stream), names the contract that actually
  generated it.
"""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from futures.series import (
    PriceObservation,
    build_additive_adjusted_series,
    build_ratio_adjusted_series,
    build_return_stream,
    build_unadjusted_series,
)
from hypothesis import given, settings
from hypothesis import strategies as st

pytestmark = pytest.mark.property

CONTRACTS = [ContractId(venue="SYNX", product_root="EQX", year=2026, month=m) for m in (3, 6, 9)]
BASE_DAY = date(2026, 1, 1)


@st.composite
def roll_schedules(draw: st.DrawFn) -> tuple[dict[date, ContractId], list[PriceObservation]]:
    total_days = draw(st.integers(min_value=2, max_value=9))
    boundary_count = draw(st.integers(min_value=0, max_value=min(2, total_days - 1)))
    boundaries = sorted(
        draw(
            st.lists(
                st.integers(min_value=1, max_value=total_days - 1),
                min_size=boundary_count,
                max_size=boundary_count,
                unique=True,
            )
        )
    )

    schedule: list[int] = []
    contract_index = 0
    boundary_iter = iter(boundaries)
    next_boundary = next(boundary_iter, None)
    for day in range(total_days):
        if next_boundary is not None and day == next_boundary:
            contract_index += 1
            next_boundary = next(boundary_iter, None)
        schedule.append(contract_index)

    front_by_date = {BASE_DAY + timedelta(days=i): CONTRACTS[idx] for i, idx in enumerate(schedule)}

    prices: list[PriceObservation] = []
    for i, day_offset in enumerate(range(total_days)):
        as_of = BASE_DAY + timedelta(days=day_offset)
        contract_id = CONTRACTS[schedule[i]]
        price = Decimal(draw(st.integers(min_value=1, max_value=10_000)))
        prices.append(PriceObservation(contract_id, as_of, price))
        # Same-day dual quote for the outgoing contract on every roll day.
        if i > 0 and schedule[i] != schedule[i - 1]:
            outgoing = CONTRACTS[schedule[i - 1]]
            outgoing_price = Decimal(draw(st.integers(min_value=1, max_value=10_000)))
            prices.append(PriceObservation(outgoing, as_of, outgoing_price))

    return front_by_date, prices


@given(data=roll_schedules())
@settings(max_examples=100)
def test_unadjusted_series_is_deterministic(data: tuple[dict[date, ContractId], list[PriceObservation]]) -> None:
    front_by_date, prices = data
    first = build_unadjusted_series(front_by_date, prices)
    second = build_unadjusted_series(front_by_date, prices)
    assert first == second


@given(data=roll_schedules())
@settings(max_examples=100)
def test_unadjusted_series_independent_of_price_list_order(
    data: tuple[dict[date, ContractId], list[PriceObservation]],
) -> None:
    front_by_date, prices = data
    forward = build_unadjusted_series(front_by_date, prices)
    backward = build_unadjusted_series(front_by_date, list(reversed(prices)))
    assert forward == backward


@given(data=roll_schedules())
@settings(max_examples=100)
def test_provenance_present_at_every_stage(
    data: tuple[dict[date, ContractId], list[PriceObservation]],
) -> None:
    front_by_date, prices = data
    unadjusted = build_unadjusted_series(front_by_date, prices)
    additive = build_additive_adjusted_series(unadjusted, prices)
    ratio = build_ratio_adjusted_series(unadjusted, prices)

    expected = [front_by_date[d] for d in sorted(front_by_date)]
    assert [o.contract_id for o in unadjusted] == expected
    assert [o.contract_id for o in additive] == expected
    assert [o.contract_id for o in ratio] == expected


@given(data=roll_schedules())
@settings(max_examples=100)
def test_additive_series_is_deterministic_and_order_independent(
    data: tuple[dict[date, ContractId], list[PriceObservation]],
) -> None:
    front_by_date, prices = data
    unadjusted = build_unadjusted_series(front_by_date, prices)
    first = build_additive_adjusted_series(unadjusted, prices)
    second = build_additive_adjusted_series(unadjusted, list(reversed(prices)))
    assert first == second


@given(data=roll_schedules())
@settings(max_examples=100)
def test_ratio_series_is_deterministic_and_order_independent(
    data: tuple[dict[date, ContractId], list[PriceObservation]],
) -> None:
    front_by_date, prices = data
    unadjusted = build_unadjusted_series(front_by_date, prices)
    first = build_ratio_adjusted_series(unadjusted, prices)
    second = build_ratio_adjusted_series(unadjusted, list(reversed(prices)))
    assert first == second


@given(data=roll_schedules())
@settings(max_examples=100)
def test_most_recent_observation_always_carries_the_identity_offset_or_factor(
    data: tuple[dict[date, ContractId], list[PriceObservation]],
) -> None:
    front_by_date, prices = data
    unadjusted = build_unadjusted_series(front_by_date, prices)
    additive = build_additive_adjusted_series(unadjusted, prices)
    ratio = build_ratio_adjusted_series(unadjusted, prices)
    assert additive[-1].adjustment_offset == Decimal(0)
    assert ratio[-1].adjustment_factor == Decimal(1)


@given(data=roll_schedules())
@settings(max_examples=100)
def test_return_stream_length_is_one_less_than_the_series(
    data: tuple[dict[date, ContractId], list[PriceObservation]],
) -> None:
    front_by_date, prices = data
    unadjusted = build_unadjusted_series(front_by_date, prices)
    ratio = build_ratio_adjusted_series(unadjusted, prices)
    returns = build_return_stream(ratio)
    assert len(returns) == max(len(ratio) - 1, 0)


@given(data=roll_schedules())
@settings(max_examples=50)
def test_is_roll_point_agrees_between_unadjusted_and_adjusted_series(
    data: tuple[dict[date, ContractId], list[PriceObservation]],
) -> None:
    front_by_date, prices = data
    unadjusted = build_unadjusted_series(front_by_date, prices)
    additive = build_additive_adjusted_series(unadjusted, prices)
    ratio = build_ratio_adjusted_series(unadjusted, prices)
    assert [o.is_roll_point for o in unadjusted] == [o.is_roll_point for o in additive]
    assert [o.is_roll_point for o in unadjusted] == [o.is_roll_point for o in ratio]
