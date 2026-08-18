"""AEGIS-079 -- calendar-spread stationarity testing."""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from research.calendar_spread import CalendarSpreadObservation
from research.stationarity import (
    MIN_OBSERVATIONS,
    InsufficientSample,
    StationarityClassification,
)
from research.stationarity import test_spread_stationarity as run_stationarity_test

pytestmark = pytest.mark.unit

NEAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
FAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
BASE_DAY = date(2026, 1, 1)


def _observations_from_spreads(spreads: list[float]) -> list[CalendarSpreadObservation]:
    return [
        CalendarSpreadObservation(
            as_of=BASE_DAY + timedelta(days=i),
            near_contract_id=NEAR,
            far_contract_id=FAR,
            near_price=Decimal(0),
            far_price=Decimal(repr(spread)),
            roll_policy_name="FixedDaysPolicy",
            far_price_provenance="test fixture",
            contract_steps=1,
        )
        for i, spread in enumerate(spreads)
    ]


def _ar1_series(mean: float, phi: float, perturbations: list[float], length: int) -> list[float]:
    """A deterministic, strongly mean-reverting AR(1)-shaped series --
    ``phi=0.3`` pulls every step 70% of the way back toward ``mean``, with a
    fixed (not literally random) perturbation cycle so the sample is fully
    reproducible."""
    values = [mean]
    for i in range(1, length):
        values.append(mean + phi * (values[-1] - mean) + perturbations[i % len(perturbations)])
    return values


def _clearly_stationary_series() -> list[float]:
    perturbations = [2, -2, 1, -1, 0.5, -0.5, 1.5, -1.5, 0.8, -0.8, 1.2, -1.2, 0.6, -0.6, 1.0, -1.0]
    return _ar1_series(mean=10.0, phi=0.3, perturbations=perturbations, length=20)


def _clearly_trending_series() -> list[float]:
    return [100.0 + 5 * i + (1 if i % 2 == 0 else -1) for i in range(20)]


def test_clearly_mean_reverting_series_is_classified_stationary() -> None:
    result = run_stationarity_test(_observations_from_spreads(_clearly_stationary_series()))
    assert result.classification == StationarityClassification.STATIONARY
    assert result.test_statistic < result.critical_value
    assert result.regression_slope < 0  # Mean reversion: a high level predicts a negative next move.


def test_clearly_trending_series_is_classified_non_stationary() -> None:
    result = run_stationarity_test(_observations_from_spreads(_clearly_trending_series()))
    assert result.classification == StationarityClassification.NON_STATIONARY
    assert result.test_statistic >= result.critical_value


def test_insufficient_sample_raises_rather_than_reporting_a_result() -> None:
    spreads = [float(i) for i in range(MIN_OBSERVATIONS - 1)]
    with pytest.raises(InsufficientSample, match="at least"):
        run_stationarity_test(_observations_from_spreads(spreads))


def test_zero_variance_lagged_series_raises_rather_than_dividing_by_zero() -> None:
    spreads = [5.0] * MIN_OBSERVATIONS
    with pytest.raises(InsufficientSample, match="zero variance"):
        run_stationarity_test(_observations_from_spreads(spreads))


def test_rejects_an_unknown_significance_level() -> None:
    with pytest.raises(ValueError, match="significance_level"):
        run_stationarity_test(
            _observations_from_spreads(_clearly_stationary_series()), significance_level="7%"
        )


def test_result_states_the_assumptions_and_caveats_not_just_the_number() -> None:
    result = run_stationarity_test(_observations_from_spreads(_clearly_stationary_series()))
    assert result.assumptions
    assert result.caveats
    assert any("not a claim" in caveat for caveat in result.caveats)
    assert result.null_hypothesis
    assert result.alternative_hypothesis
