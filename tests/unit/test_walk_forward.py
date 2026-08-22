"""AEGIS-140, AEGIS-141 -- rolling walk-forward and expanding-window
boundary verification."""

from __future__ import annotations

from datetime import date, timedelta

import pytest
from validation.walk_forward import WalkForwardFold, expanding_window, rolling_walk_forward

pytestmark = pytest.mark.unit


def _dates(n: int) -> list[date]:
    start = date(2026, 1, 1)
    return [start + timedelta(days=i) for i in range(n)]


def test_rolling_walk_forward_produces_literal_expected_folds() -> None:
    dates = _dates(10)
    folds = rolling_walk_forward(dates, train_window=4, test_window=2)
    assert folds == [
        WalkForwardFold(train_start=dates[0], train_end=dates[3], test_start=dates[4], test_end=dates[5]),
        WalkForwardFold(train_start=dates[2], train_end=dates[5], test_start=dates[6], test_end=dates[7]),
        WalkForwardFold(train_start=dates[4], train_end=dates[7], test_start=dates[8], test_end=dates[9]),
    ]


def test_rolling_walk_forward_train_window_size_is_constant_across_folds() -> None:
    dates = _dates(20)
    folds = rolling_walk_forward(dates, train_window=5, test_window=3)
    assert len(folds) > 1
    for fold in folds:
        assert (fold.train_end - fold.train_start).days == 4  # 5 observations, inclusive.


def test_rolling_walk_forward_never_overlaps_train_and_test() -> None:
    dates = _dates(30)
    for fold in rolling_walk_forward(dates, train_window=7, test_window=4, step=2):
        assert fold.train_end < fold.test_start


def test_expanding_window_train_start_is_constant_and_train_end_grows() -> None:
    dates = _dates(12)
    folds = expanding_window(dates, initial_train_window=4, test_window=2)
    assert len(folds) >= 2
    assert all(fold.train_start == dates[0] for fold in folds)
    train_ends = [fold.train_end for fold in folds]
    assert train_ends == sorted(train_ends)
    assert len(set(train_ends)) == len(train_ends)  # Strictly growing, no repeats.


def test_expanding_window_literal_expected_folds() -> None:
    dates = _dates(8)
    folds = expanding_window(dates, initial_train_window=4, test_window=2)
    assert folds == [
        WalkForwardFold(train_start=dates[0], train_end=dates[3], test_start=dates[4], test_end=dates[5]),
        WalkForwardFold(train_start=dates[0], train_end=dates[5], test_start=dates[6], test_end=dates[7]),
    ]


def test_expanding_window_truncates_the_final_fold_instead_of_dropping_it() -> None:
    dates = _dates(9)  # 4 initial + folds of 2 leaves one trailing observation.
    folds = expanding_window(dates, initial_train_window=4, test_window=2)
    assert folds[-1].test_end == dates[-1]  # The last date is covered, not dropped.


def test_no_leakage_train_end_strictly_precedes_test_start_in_every_fold() -> None:
    dates = _dates(25)
    for fold in [*rolling_walk_forward(dates, 6, 3), *expanding_window(dates, 6, 3)]:
        assert fold.train_end < fold.test_start


def test_fold_construction_rejects_non_monotonic_boundaries() -> None:
    with pytest.raises(ValueError, match="train_start <= train_end"):
        WalkForwardFold(
            train_start=date(2026, 1, 5),
            train_end=date(2026, 1, 1),
            test_start=date(2026, 1, 6),
            test_end=date(2026, 1, 7),
        )
