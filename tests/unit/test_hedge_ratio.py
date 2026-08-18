"""AEGIS-078 -- hedge-ratio estimation, static and rolling (ADR-0026)."""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from research.calendar_spread import CalendarSpreadObservation
from research.hedge_ratio import (
    InsufficientObservations,
    rolling_hedge_ratio,
    static_hedge_ratio,
)

pytestmark = pytest.mark.unit

NEAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
FAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
BASE_DAY = date(2026, 1, 1)


def _observations(near_prices: list[Decimal], far_prices: list[Decimal]) -> list[CalendarSpreadObservation]:
    return [
        CalendarSpreadObservation(
            as_of=BASE_DAY + timedelta(days=i),
            near_contract_id=NEAR,
            far_contract_id=FAR,
            near_price=near,
            far_price=far,
            roll_policy_name="FixedDaysPolicy",
            far_price_provenance="test fixture",
            contract_steps=1,
        )
        for i, (near, far) in enumerate(zip(near_prices, far_prices, strict=True))
    ]


def test_static_hedge_ratio_recovers_an_exact_linear_relationship() -> None:
    # far = 2 * near + 5 exactly: the OLS slope must recover 2 exactly.
    near = [Decimal(x) for x in (10, 20, 30, 40)]
    far = [2 * x + 5 for x in near]
    observations = _observations(near, far)
    assert static_hedge_ratio(observations) == Decimal(2)


def test_static_hedge_ratio_needs_at_least_two_observations() -> None:
    observations = _observations([Decimal(10)], [Decimal(20)])
    with pytest.raises(InsufficientObservations, match="at least 2"):
        static_hedge_ratio(observations)


def test_static_hedge_ratio_rejects_zero_variance_near_series() -> None:
    observations = _observations([Decimal(10), Decimal(10)], [Decimal(20), Decimal(25)])
    with pytest.raises(InsufficientObservations, match="zero variance"):
        static_hedge_ratio(observations)


def test_rolling_hedge_ratio_uses_only_prior_observations_never_the_current_one() -> None:
    # far = 3 * near for indices 0..3, then the relationship changes sharply
    # at index 4 (far = 100 * near). If index 4's own value leaked into its
    # own window, its ratio would be pulled toward 100; leakage-free, it must
    # still reflect only the prior (ratio == 3) window.
    near = [Decimal(x) for x in (1, 2, 3, 4, 5)]
    far = [3 * near[0], 3 * near[1], 3 * near[2], 3 * near[3], 100 * near[4]]
    observations = _observations(near, far)

    results = rolling_hedge_ratio(observations, window=4)

    assert [r.hedge_ratio for r in results[:4]] == [None, None, None, None]
    assert results[4].hedge_ratio == Decimal(3)  # Not 100 -- index 4 never entered its own window.


def test_rolling_hedge_ratio_reports_none_when_the_prior_window_has_zero_variance() -> None:
    near = [Decimal(10), Decimal(10), Decimal(10)]
    far = [Decimal(20), Decimal(20), Decimal(30)]
    observations = _observations(near, far)
    results = rolling_hedge_ratio(observations, window=2)
    assert results[2].hedge_ratio is None


def test_rolling_hedge_ratio_rejects_window_below_two() -> None:
    with pytest.raises(ValueError, match="window must be >= 2"):
        rolling_hedge_ratio(_observations([Decimal(1)], [Decimal(1)]), window=1)
