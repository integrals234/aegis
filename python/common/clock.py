"""Time domains and injectable clocks for the Python side (ADR-0002).

This is the peer of ``cpp/common/time.hpp`` and ``cpp/common/clock.hpp``, and it
exists for the same reason: ``docs/ARCHITECTURE.md`` names seven distinct clocks,
all of them 64-bit integers, so nothing in the representation stops a research
script from subtracting a monotonic reading from an exchange timestamp and
reporting the difference as latency.

C++ rejects that at compile time. Python cannot, so the domains are separate
classes whose subtraction refuses to cross domains at runtime, and the error
names both domains rather than producing a plausible number.

Clocks are injected here too: a component that reads the system clock directly
produces different output on every run, which is incompatible with the
byte-identical canonical output AEGIS-005 requires. There is deliberately no
module-level default clock.
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import ClassVar, Protocol

Nanos = int

NANOS_PER_MICRO = 1_000
NANOS_PER_MILLI = 1_000_000
NANOS_PER_SECOND = 1_000_000_000


@dataclass(frozen=True, slots=True, order=True)
class Duration:
    """A signed span of time, independent of any clock domain.

    Signed on purpose: a clock stepping backwards, or an event stamped before its
    predecessor, must stay visible as a negative number rather than wrapping into
    an implausibly large positive one.
    """

    nanos: int

    def __add__(self, other: Duration) -> Duration:
        return Duration(self.nanos + other.nanos)

    def __sub__(self, other: Duration) -> Duration:
        return Duration(self.nanos - other.nanos)

    def __neg__(self) -> Duration:
        return Duration(-self.nanos)

    @property
    def micros(self) -> float:
        return self.nanos / NANOS_PER_MICRO

    @property
    def millis(self) -> float:
        return self.nanos / NANOS_PER_MILLI

    @property
    def seconds(self) -> float:
        return self.nanos / NANOS_PER_SECOND


@dataclass(frozen=True, slots=True, order=True)
class Timestamp:
    """A point in time within exactly one clock domain.

    Subclasses name the domain. Two subclasses are never interchangeable, and
    subtraction across them raises rather than returning a number.
    """

    nanos: int

    domain: ClassVar[str] = "unspecified"
    serializable: ClassVar[bool] = True

    def __sub__(self, other: Timestamp | Duration) -> Duration | Timestamp:
        if isinstance(other, Duration):
            return type(self)(self.nanos - other.nanos)
        if type(other) is not type(self):
            raise TypeError(
                f"cannot subtract {type(other).__name__} ({other.domain} domain) from "
                f"{type(self).__name__} ({self.domain} domain): these are different clocks, "
                "and the difference between them is not a duration. "
                "Measure latency with elapsed() over two MonotonicTime readings."
            )
        return Duration(self.nanos - other.nanos)

    def __add__(self, delta: Duration) -> Timestamp:
        return type(self)(self.nanos + delta.nanos)


class EventTime(Timestamp):
    """Source/exchange event timestamp."""

    domain: ClassVar[str] = "event"


class ReceiveTime(Timestamp):
    """Participant receipt timestamp."""

    domain: ClassVar[str] = "receive"


class DecisionTime(Timestamp):
    """Strategy or human action timestamp."""

    domain: ClassVar[str] = "decision"


class SubmitTime(Timestamp):
    """OMS/gateway submission timestamp."""

    domain: ClassVar[str] = "submit"


class ExchangeTime(Timestamp):
    """Exchange processing timestamp."""

    domain: ClassVar[str] = "exchange"


class AckTime(Timestamp):
    """Acknowledgement/execution timestamp."""

    domain: ClassVar[str] = "ack"


class MonotonicTime(Timestamp):
    """Local monotonic reading. Latency measurement only, never persisted.

    Its origin is unspecified and differs between processes and reboots, so a
    persisted monotonic reading replays to a different value on the next run —
    which is exactly what breaks byte-identical canonical output.
    """

    domain: ClassVar[str] = "monotonic"
    serializable: ClassVar[bool] = False


def serialize_nanos(stamp: Timestamp) -> int:
    """Return the nanoseconds of a timestamp that may enter a persisted record."""
    if not stamp.serializable:
        raise TypeError(
            f"{type(stamp).__name__} must never be serialized: a monotonic reading is "
            "meaningful only within one process run, so persisting one produces a record "
            "that replays to a different value (AEGIS-005)."
        )
    return stamp.nanos


def elapsed(start: MonotonicTime, end: MonotonicTime) -> Duration:
    """Latency between two monotonic readings taken in the same process."""
    return Duration(end.nanos - start.nanos)


def micros(count: int) -> Duration:
    return Duration(count * NANOS_PER_MICRO)


def millis(count: int) -> Duration:
    return Duration(count * NANOS_PER_MILLI)


def seconds(count: int) -> Duration:
    return Duration(count * NANOS_PER_SECOND)


class WallClock(Protocol):
    """Source of UTC wall-clock readings."""

    def now_utc(self) -> Nanos: ...


class SteadyClock(Protocol):
    """Source of monotonic readings, for latency measurement only."""

    def now(self) -> MonotonicTime: ...


class SystemWallClock:
    """Wall clock backed by the operating system."""

    def now_utc(self) -> Nanos:
        return time.time_ns()


class SystemSteadyClock:
    """Monotonic clock backed by the operating system."""

    def now(self) -> MonotonicTime:
        return MonotonicTime(time.monotonic_ns())


class ManualClock:
    """Wall clock a test drives by hand.

    Not a convenience: a fixture whose timestamps come from the system clock
    cannot be asserted byte for byte, so the determinism harness would have
    nothing stable to hash.
    """

    def __init__(self, start: Nanos = 0) -> None:
        self._now = start

    def now_utc(self) -> Nanos:
        return self._now

    def set(self, nanos: Nanos) -> None:
        self._now = nanos

    def advance(self, delta: Duration) -> None:
        self._now += delta.nanos


class ManualSteadyClock:
    """Monotonic clock a test drives by hand; never moves backwards."""

    def __init__(self, start: Nanos = 0) -> None:
        self._now = start

    def now(self) -> MonotonicTime:
        return MonotonicTime(self._now)

    def advance(self, delta: Duration) -> None:
        self._now += max(delta.nanos, 0)


def stamp[T: Timestamp](clock: WallClock, domain: type[T]) -> T:
    """Take a wall-clock reading into a named domain.

    The domain is chosen at the call site rather than baked into the clock, so
    which clock a stamp came from stays visible where it is decided.
    """
    if not domain.serializable:
        raise TypeError(
            f"{domain.__name__} is a monotonic domain and cannot be taken from a wall clock; "
            "use SteadyClock.now()"
        )
    return domain(clock.now_utc())
