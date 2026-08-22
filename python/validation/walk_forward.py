"""Rolling walk-forward and expanding-window testing (AEGIS-140, AEGIS-141).

Both fold generators share one invariant, enforced by construction rather
than checked afterward: ``train_end < test_start`` strictly, for every fold.
A test's own timestamp can never fall inside the training window that
produced the model being tested on it -- the leakage `` tests/unit/
test_walk_forward.py`` exists to catch is exactly a caller passing overlapping
windows some other way, not a defect this module's own arithmetic could
produce.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date

__all__ = ["WalkForwardFold", "expanding_window", "rolling_walk_forward"]


@dataclass(frozen=True, slots=True)
class WalkForwardFold:
    train_start: date
    train_end: date
    test_start: date
    test_end: date

    def __post_init__(self) -> None:
        if not (self.train_start <= self.train_end < self.test_start <= self.test_end):
            raise ValueError(
                "fold boundaries must satisfy train_start <= train_end < test_start <= test_end"
            )


def rolling_walk_forward(
    dates: Sequence[date], train_window: int, test_window: int, step: int | None = None
) -> list[WalkForwardFold]:
    """Sliding windows: both ``train_start`` and ``train_end`` advance each
    fold, so the training window's *size* (``train_window`` observations) is
    constant across folds -- the classic rolling walk-forward. ``step``
    defaults to ``test_window`` (non-overlapping test windows); a caller
    passing a smaller step deliberately overlaps test windows across folds,
    which this function permits (it is a research choice, not a leakage
    bug) but never lets `` train`` overlap the fold's own test window.
    """
    if train_window <= 0 or test_window <= 0:
        raise ValueError("train_window and test_window must be positive")
    step = test_window if step is None else step
    if step <= 0:
        raise ValueError("step must be positive")

    folds: list[WalkForwardFold] = []
    start = 0
    n = len(dates)
    while start + train_window + test_window <= n:
        train_start = dates[start]
        train_end = dates[start + train_window - 1]
        test_start = dates[start + train_window]
        test_end = dates[start + train_window + test_window - 1]
        folds.append(
            WalkForwardFold(
                train_start=train_start, train_end=train_end, test_start=test_start, test_end=test_end
            )
        )
        start += step
    return folds


def expanding_window(
    dates: Sequence[date], initial_train_window: int, test_window: int, step: int | None = None
) -> list[WalkForwardFold]:
    """Expanding windows: ``train_start`` is constant (``dates[0]``) across
    every fold; only ``train_end`` grows, by ``step`` observations per fold
    (default ``test_window``, i.e. non-overlapping test windows). The final
    fold's test window is truncated to whatever remains rather than dropped,
    so the last observations in ``dates`` are never silently excluded from
    every fold.
    """
    if initial_train_window <= 0 or test_window <= 0:
        raise ValueError("initial_train_window and test_window must be positive")
    step = test_window if step is None else step
    if step <= 0:
        raise ValueError("step must be positive")

    folds: list[WalkForwardFold] = []
    n = len(dates)
    train_end_index = initial_train_window - 1
    while train_end_index + 1 < n:
        test_start_index = train_end_index + 1
        test_end_index = min(train_end_index + test_window, n - 1)
        folds.append(
            WalkForwardFold(
                train_start=dates[0],
                train_end=dates[train_end_index],
                test_start=dates[test_start_index],
                test_end=dates[test_end_index],
            )
        )
        train_end_index += step
    return folds
