"""AEGIS-080 -- leakage-free rolling z-score Python reference (ADR-0026).

Same edge cases and arithmetic as `cpp/statistics/rolling_zscore.hpp`
(`tests/cpp/unit/test_calendar_spread_strategy.cpp` exercises the C++ side
against the identical spread sequence used here) -- a later batch
cross-checks the two directly through the compiled bindings; this test
verifies the Python reference against its own textbook definition.
"""

from __future__ import annotations

import pytest
from research.signal_reference import WindowProvenance, rolling_zscore_reference

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


def test_supplying_a_timing_sink_does_not_change_the_numeric_scores() -> None:
    spreads = [0.50, 0.55, 0.60, 0.65, 2.50, 0.70]
    without_sink = list(rolling_zscore_reference(spreads, window=20))
    with_sink = list(rolling_zscore_reference(spreads, window=20, timing_sink=lambda _: None))
    assert without_sink == with_sink


def test_provenance_reports_the_actual_consumed_window_not_a_feature_index_formula() -> None:
    # AEGIS-152 closure repair (round 2): the first fix still computed
    # fitting_window_end_index = index - 1 as a constant expression, so it
    # could never disagree with a leaking estimator. This test proves the
    # replacement instead: hand-worked ground truth for a small, fully
    # traceable fixture (window=3 over 6 values), independent of whatever
    # formula the production code happens to use internally.
    #
    #   i=0: nothing consumed yet -> empty window
    #   i=1: window holds index 0
    #   i=2: window holds indices 0,1
    #   i=3: window holds indices 0,1,2 (full at maxlen=3)
    #   i=4: window holds indices 1,2,3 (index 0 evicted)
    #   i=5: window holds indices 2,3,4 (index 1 evicted)
    values = [10.0, 20.0, 15.0, 30.0, 5.0, 25.0]
    expected_by_index: dict[int, tuple[int, int] | None] = {
        0: None,
        1: (0, 0),
        2: (0, 1),
        3: (0, 2),
        4: (1, 3),
        5: (2, 4),
    }

    records: list[WindowProvenance] = []
    list(rolling_zscore_reference(values, window=3, timing_sink=records.append))

    assert len(records) == 6
    for record in records:
        expected = expected_by_index[record.feature_index]
        if expected is None:
            assert record.fitting_window_start_index > record.fitting_window_end_index
        else:
            expected_start, expected_end = expected
            assert record.fitting_window_start_index == expected_start
            assert record.fitting_window_end_index == expected_end
