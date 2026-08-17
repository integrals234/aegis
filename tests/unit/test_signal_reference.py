"""AEGIS-080 -- leakage-free rolling z-score Python reference (ADR-0026).

Same edge cases and arithmetic as `cpp/statistics/rolling_zscore.hpp`
(`tests/cpp/unit/test_calendar_spread_strategy.cpp` exercises the C++ side
against the identical spread sequence used here) -- a later batch
cross-checks the two directly through the compiled bindings; this test
verifies the Python reference against its own textbook definition.
"""

from __future__ import annotations

import pytest
from research.signal_reference import rolling_zscore_reference

pytestmark = pytest.mark.unit


def test_first_observation_never_scores_against_itself() -> None:
    scores = list(rolling_zscore_reference([1000.0, 1.0, 2.0], window=5))
    assert scores[0] == 0.0  # count == 0 prior: the documented edge case.


def test_second_observation_also_reports_the_documented_zero_edge_case() -> None:
    scores = list(rolling_zscore_reference([1.0, 2.0, 3.0], window=5))
    assert scores[0] == 0.0
    assert scores[1] == 0.0  # count == 1 prior: still fewer than two observations.


def test_matches_cpp_rolling_zscore_arithmetic_exactly() -> None:
    """The same six-value sequence, and the same expected scores,
    `tests/cpp/unit/test_calendar_spread_strategy.cpp` asserts against the
    compiled C++ `RollingZScore` -- proving the two independent
    implementations agree, not merely that each agrees with itself."""
    spreads = [0.50, 0.55, 0.60, 0.65, 2.50, 0.70]
    scores = list(rolling_zscore_reference(spreads, window=20))
    expected = [
        0.0,
        0.0,
        2.1213203435596393,
        2.000000000000002,
        29.821971765797112,
        -0.3013796514749198,
    ]
    for actual, want in zip(scores, expected, strict=True):
        assert actual == pytest.approx(want, abs=1e-9)


def test_zero_variance_window_scores_zero_rather_than_dividing_by_zero() -> None:
    scores = list(rolling_zscore_reference([5.0, 5.0, 5.0], window=5))
    assert scores[2] == 0.0  # Prior window [5.0, 5.0]: zero variance, defined edge case.


def test_window_must_be_positive() -> None:
    with pytest.raises(ValueError, match="window must be > 0"):
        list(rolling_zscore_reference([1.0], window=0))
