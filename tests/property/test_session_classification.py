"""M2 slice 3 -- invariants AEGIS-013's classifier must hold for every
timestamp, not just the handful of examples in test_futures_calendars.py.

The invariant the roll/series/ingestion layers all depend on: a timestamp
classifies into exactly one valid session state or CLOSED, never two
contradictory ones, and classification is a pure function of
``(as_of_ns, template)`` -- no hidden dependence on call order, process, or
hash seed.
"""

from __future__ import annotations

import bisect
import itertools
from datetime import UTC, datetime
from pathlib import Path

import pytest
from futures.calendars import CalendarRegistry, SessionState, load_calendar_registry
from hypothesis import given, settings
from hypothesis import strategies as st

pytestmark = pytest.mark.property

ROOT = Path(__file__).resolve().parents[2]


def _window_ns() -> tuple[int, int]:
    start = int(datetime(2025, 1, 1, tzinfo=UTC).timestamp() * 1_000_000_000)
    end = int(datetime(2028, 1, 1, tzinfo=UTC).timestamp() * 1_000_000_000)
    return start, end


@pytest.fixture(scope="module")
def registry() -> CalendarRegistry:
    return load_calendar_registry(ROOT)


_START_NS, _END_NS = _window_ns()
timestamps = st.integers(min_value=_START_NS, max_value=_END_NS)
templates = st.sampled_from(("synx_equity_index_rth", "synx_energy_extended", "synx_rates_globex"))


def _reference_classify(registry: CalendarRegistry, as_of_ns: int, template: str) -> str | None:
    """Brute-force linear scan, independent of CalendarRegistry's bisect path."""
    for interval in registry.intervals(template):
        if interval.start_ns <= as_of_ns < interval.end_ns:
            return interval.name
    return None


@given(as_of_ns=timestamps, template=templates)
@settings(max_examples=300)
def test_classification_matches_linear_scan_reference(
    registry: CalendarRegistry, as_of_ns: int, template: str
) -> None:
    result = registry.classify(as_of_ns, template)
    expected_name = _reference_classify(registry, as_of_ns, template)
    if expected_name is None:
        assert result.state is SessionState.CLOSED
        assert result.session_name is None
    else:
        assert result.session_name == expected_name


@given(as_of_ns=timestamps, template=templates)
@settings(max_examples=300)
def test_classification_is_pure_and_repeatable(
    registry: CalendarRegistry, as_of_ns: int, template: str
) -> None:
    first = registry.classify(as_of_ns, template)
    second = registry.classify(as_of_ns, template)
    assert first == second


@given(as_of_ns=timestamps, template=templates)
@settings(max_examples=300)
def test_at_most_one_interval_contains_any_instant(
    registry: CalendarRegistry, as_of_ns: int, template: str
) -> None:
    """The property the whole calendar design rests on: never two
    contradictory session states for the same instant."""
    containing = [iv for iv in registry.intervals(template) if iv.start_ns <= as_of_ns < iv.end_ns]
    assert len(containing) <= 1


@given(template=templates)
@settings(max_examples=10)
def test_committed_intervals_are_sorted_and_non_overlapping(
    registry: CalendarRegistry, template: str
) -> None:
    intervals = registry.intervals(template)
    starts = [iv.start_ns for iv in intervals]
    assert starts == sorted(starts)
    for prev, curr in itertools.pairwise(intervals):
        assert curr.start_ns >= prev.end_ns


@given(as_of_ns=timestamps, template=templates)
@settings(max_examples=200)
def test_boundary_bisect_agrees_with_manual_bisect(
    registry: CalendarRegistry, as_of_ns: int, template: str
) -> None:
    """classify()'s internal bisect_right - 1 agrees with an independently
    written bisect against the same sorted start list."""
    starts = [iv.start_ns for iv in registry.intervals(template)]
    idx = bisect.bisect_right(starts, as_of_ns) - 1
    result = registry.classify(as_of_ns, template)
    intervals = registry.intervals(template)
    if 0 <= idx < len(intervals) and intervals[idx].start_ns <= as_of_ns < intervals[idx].end_ns:
        assert result.session_name == intervals[idx].name
    else:
        assert result.state is SessionState.CLOSED
