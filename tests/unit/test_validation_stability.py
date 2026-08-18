"""AEGIS-142 -- parameter stability surfaces evaluate a neighbourhood, not
only the best point."""

from __future__ import annotations

from decimal import Decimal

import pytest
from validation._fixtures import make_synthetic_spread_series
from validation.stability import compute_parameter_stability_surface

pytestmark = pytest.mark.unit


def test_every_grid_point_is_persisted_not_only_the_best() -> None:
    observations = make_synthetic_spread_series("EQX", seed=1)
    surface = compute_parameter_stability_surface(
        observations, Decimal(1),
        zscore_windows=(10, 20), entry_thresholds=(1.5, 2.0, 2.5), exit_thresholds=(0.5,),
    )

    assert len(surface.points) == 2 * 3 * 1  # Every combination, none skipped.
    assert surface.best in surface.points
    # A report containing only the optimum is the documented failure; this
    # asserts the neighbourhood itself is retrievable.
    records = surface.as_records()
    assert len(records) == len(surface.points)
    assert sum(1 for r in records if r["is_best"]) == 1


def test_reproducible_given_the_same_inputs() -> None:
    observations = make_synthetic_spread_series("EQX", seed=1)
    first = compute_parameter_stability_surface(
        observations, Decimal(1), zscore_windows=(20,), entry_thresholds=(2.0,), exit_thresholds=(0.5,)
    )
    second = compute_parameter_stability_surface(
        observations, Decimal(1), zscore_windows=(20,), entry_thresholds=(2.0,), exit_thresholds=(0.5,)
    )
    assert first.points == second.points
    assert first.metric_stdev == second.metric_stdev


def test_dispersion_is_zero_for_a_single_point_grid() -> None:
    observations = make_synthetic_spread_series("EQX", seed=1)
    surface = compute_parameter_stability_surface(
        observations, Decimal(1), zscore_windows=(20,), entry_thresholds=(2.0,), exit_thresholds=(0.5,)
    )
    assert surface.metric_stdev == 0.0


def test_structurally_invalid_exit_over_entry_combinations_are_skipped() -> None:
    observations = make_synthetic_spread_series("EQX", seed=1)
    surface = compute_parameter_stability_surface(
        observations, Decimal(1),
        zscore_windows=(20,), entry_thresholds=(1.0,), exit_thresholds=(0.5, 1.5),  # 1.5 >= entry_threshold 1.0.
    )
    assert len(surface.points) == 1  # Only exit_threshold=0.5 is valid.


def test_an_empty_grid_raises_rather_than_silently_reporting_nothing() -> None:
    observations = make_synthetic_spread_series("EQX", seed=1)
    with pytest.raises(ValueError, match="no valid"):
        compute_parameter_stability_surface(
            observations, Decimal(1), zscore_windows=(20,), entry_thresholds=(1.0,), exit_thresholds=(2.0,)
        )
