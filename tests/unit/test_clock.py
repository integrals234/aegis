"""ADR-0002 — Python clock domains must refuse to mix, and clocks are injected.

The C++ peer rejects a cross-domain subtraction at compile time. Python cannot,
so the equivalent guarantee is a runtime refusal, and these tests are what make
that refusal real rather than documented.
"""

from __future__ import annotations

import pytest
from common.clock import (
    AckTime,
    Duration,
    EventTime,
    ExchangeTime,
    ManualClock,
    ManualSteadyClock,
    MonotonicTime,
    ReceiveTime,
    SubmitTime,
    SystemSteadyClock,
    SystemWallClock,
    elapsed,
    micros,
    millis,
    serialize_nanos,
    stamp,
)

pytestmark = pytest.mark.unit


def test_same_domain_difference_is_a_duration():
    assert (EventTime(1_500) - EventTime(1_000)) == Duration(500)
    assert (EventTime(1_000) - EventTime(1_500)) == Duration(-500)


@pytest.mark.parametrize(
    ("left", "right"),
    [
        (AckTime(10), MonotonicTime(5)),
        (EventTime(10), ReceiveTime(5)),
        (SubmitTime(10), ExchangeTime(5)),
        (ReceiveTime(10), AckTime(5)),
    ],
)
def test_cross_domain_subtraction_raises(left, right):
    """Mixing clocks must fail loudly, not produce a plausible latency figure."""
    with pytest.raises(TypeError) as excinfo:
        left - right
    message = str(excinfo.value)
    assert left.domain in message
    assert right.domain in message


def test_cross_domain_error_points_at_the_supported_measurement():
    with pytest.raises(TypeError, match="elapsed"):
        AckTime(10) - MonotonicTime(5)


def test_duration_is_signed_so_backwards_steps_stay_visible():
    backwards = EventTime(5) - EventTime(9)
    assert backwards.nanos == -4


def test_timestamp_shifts_by_a_duration():
    assert (ReceiveTime(10_000) + micros(5)) == ReceiveTime(15_000)
    assert (ReceiveTime(10_000) - micros(5)) == ReceiveTime(5_000)


def test_monotonic_time_cannot_be_serialized():
    """A persisted monotonic reading replays to a different value (AEGIS-005)."""
    with pytest.raises(TypeError, match="never be serialized"):
        serialize_nanos(MonotonicTime(123))


@pytest.mark.parametrize(
    "domain", [EventTime, ReceiveTime, SubmitTime, ExchangeTime, AckTime]
)
def test_wall_clock_domains_are_serializable(domain):
    assert serialize_nanos(domain(42)) == 42


def test_manual_clock_produces_exactly_the_time_a_test_asks_for():
    clock = ManualClock(1_700_000_000_000_000_000)
    assert clock.now_utc() == 1_700_000_000_000_000_000

    clock.advance(millis(250))
    assert stamp(clock, EventTime) == EventTime(1_700_000_000_250_000_000)


def test_manual_steady_clock_never_moves_backwards():
    clock = ManualSteadyClock(100)
    clock.advance(Duration(-50))
    assert clock.now() == MonotonicTime(100)

    clock.advance(Duration(25))
    assert clock.now() == MonotonicTime(125)


def test_elapsed_measures_between_two_monotonic_readings():
    clock = ManualSteadyClock()
    start = clock.now()
    clock.advance(micros(42))
    assert elapsed(start, clock.now()) == Duration(42_000)


def test_stamp_refuses_to_take_a_monotonic_reading_from_a_wall_clock():
    with pytest.raises(TypeError, match="SteadyClock"):
        stamp(ManualClock(5), MonotonicTime)


def test_system_clocks_satisfy_their_protocols():
    """The real clocks must be substitutable for the manual ones."""
    wall = SystemWallClock()
    steady = SystemSteadyClock()
    assert isinstance(wall.now_utc(), int)
    assert isinstance(steady.now(), MonotonicTime)

    first = steady.now()
    second = steady.now()
    assert elapsed(first, second).nanos >= 0


def test_duration_unit_conversions():
    assert millis(1).nanos == 1_000_000
    assert millis(1).micros == 1_000.0
    assert millis(1).seconds == pytest.approx(0.001)
