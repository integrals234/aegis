"""Metrics registry and health contract (AEGIS-238).

AEGIS-238 asks for health, queue depth, dropped/backpressured events, latency
and risk status. This module delivers the registry and the health contract; it
deliberately does **not** pre-register `queue_depth`, `latency` or `risk_status`.

Registering a metric with nothing feeding it produces a dashboard that reads
zero and an operator who believes zero. A gauge that has never been written is
indistinguishable from a queue that is genuinely empty, and the difference
matters exactly when something is wrong. So a metric appears here when its
producer does: queue depth with the bounded queues (M1, AEGIS-046/048), latency
with the execution path (M3, AEGIS-113), risk status with the risk engine
(M5, AEGIS-137).

The registry is an instance. A process-global registry would be mutable state
touched from every book partition, which violates the single-writer rule
(AEGIS-047) and would surface as nondeterminism inside the very determinism
check meant to catch it.

Reads are pull-based snapshots: a caller gets an immutable copy rather than a
live handle, so an observer can never mutate what it is observing.
"""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from enum import StrEnum

from common.clock import Duration, MonotonicTime, Nanos, SteadyClock

SNAPSHOT_SCHEMA_VERSION = 1

# Quantiles reported for every histogram. docs/BENCHMARK_POLICY.md forbids
# quoting mean latency alone, so the tail is not optional here either.
DEFAULT_QUANTILES: tuple[float, ...] = (0.5, 0.95, 0.99, 0.999)


class MetricKind(StrEnum):
    COUNTER = "counter"
    GAUGE = "gauge"
    HISTOGRAM = "histogram"


class HealthState(StrEnum):
    """Three states, because "not healthy" is two different operational answers.

    DEGRADED means still serving with reduced capability; UNHEALTHY means not
    serving. Collapsing them forces an operator to guess which one happened.
    """

    HEALTHY = "healthy"
    DEGRADED = "degraded"
    UNHEALTHY = "unhealthy"


class MetricError(ValueError):
    """A metric was misused in a way that would corrupt what it reports."""


@dataclass(frozen=True, slots=True)
class HistogramSnapshot:
    count: int
    total: float
    minimum: float
    maximum: float
    quantiles: Mapping[str, float]

    @property
    def mean(self) -> float:
        return self.total / self.count if self.count else 0.0


@dataclass(frozen=True, slots=True)
class MetricsSnapshot:
    """An immutable point-in-time view of a registry."""

    schema_version: int
    counters: Mapping[str, int]
    gauges: Mapping[str, float]
    histograms: Mapping[str, HistogramSnapshot]

    def to_record(self) -> dict[str, object]:
        """Serialize deterministically: sorted keys, no clock reading inside."""
        return {
            "schema_version": self.schema_version,
            "counters": dict(sorted(self.counters.items())),
            "gauges": dict(sorted(self.gauges.items())),
            "histograms": {
                name: {
                    "count": h.count,
                    "total": h.total,
                    "min": h.minimum,
                    "max": h.maximum,
                    "quantiles": dict(sorted(h.quantiles.items())),
                }
                for name, h in sorted(self.histograms.items())
            },
        }


class Counter:
    """A monotonically increasing count."""

    def __init__(self, name: str) -> None:
        self.name = name
        self._value = 0

    def increment(self, amount: int = 1) -> None:
        if amount < 0:
            # A counter that can go down is a gauge wearing a disguise, and every
            # rate computed from it silently becomes wrong.
            raise MetricError(f"counter {self.name!r} cannot decrease (got {amount}); use a gauge")
        self._value += amount

    @property
    def value(self) -> int:
        return self._value


class Gauge:
    """A value that goes up and down."""

    def __init__(self, name: str) -> None:
        self.name = name
        self._value = 0.0

    def set(self, value: float) -> None:
        if math.isnan(value):
            raise MetricError(f"gauge {self.name!r} cannot be set to NaN")
        self._value = value

    def add(self, delta: float) -> None:
        self.set(self._value + delta)

    @property
    def value(self) -> float:
        return self._value


class Histogram:
    """A distribution of observations, reported with its tail.

    Stores every observation. That is the right trade at M0 — exact quantiles,
    no bucket-boundary decisions to defend — and it is bounded by the number of
    observations a test or harness makes. A hot-path histogram belongs to M8,
    with a benchmark justifying whatever approximation it uses.
    """

    def __init__(self, name: str, quantiles: Sequence[float] = DEFAULT_QUANTILES) -> None:
        for quantile in quantiles:
            if not 0.0 < quantile < 1.0:
                raise MetricError(f"quantile {quantile} must lie strictly between 0 and 1")
        self.name = name
        self._quantiles = tuple(quantiles)
        self._observations: list[float] = []

    def observe(self, value: float) -> None:
        if math.isnan(value):
            raise MetricError(f"histogram {self.name!r} cannot observe NaN")
        self._observations.append(value)

    def observe_duration(self, duration: Duration) -> None:
        self.observe(float(duration.nanos))

    def snapshot(self) -> HistogramSnapshot:
        if not self._observations:
            return HistogramSnapshot(0, 0.0, 0.0, 0.0, {})
        ordered = sorted(self._observations)
        return HistogramSnapshot(
            count=len(ordered),
            total=math.fsum(ordered),
            minimum=ordered[0],
            maximum=ordered[-1],
            quantiles={f"p{q * 100:g}": _quantile(ordered, q) for q in self._quantiles},
        )


def _quantile(ordered: Sequence[float], quantile: float) -> float:
    """Nearest-rank quantile.

    Nearest-rank rather than interpolated: an interpolated p99.9 reports a
    latency no request actually experienced, which is not what a tail-latency
    claim is supposed to mean.
    """
    index = math.ceil(quantile * len(ordered)) - 1
    return ordered[max(0, min(index, len(ordered) - 1))]


@dataclass
class HealthCheck:
    """A named check plus the reason its current state holds."""

    name: str
    state: HealthState = HealthState.HEALTHY
    detail: str = ""


@dataclass
class HealthReport:
    checks: Mapping[str, HealthCheck] = field(default_factory=dict)

    @property
    def state(self) -> HealthState:
        """The worst state among the checks: a component is only as healthy as
        its unhealthiest part, and averaging would hide the failure."""
        if any(c.state is HealthState.UNHEALTHY for c in self.checks.values()):
            return HealthState.UNHEALTHY
        if any(c.state is HealthState.DEGRADED for c in self.checks.values()):
            return HealthState.DEGRADED
        return HealthState.HEALTHY

    def to_record(self) -> dict[str, object]:
        return {
            "state": self.state.value,
            "checks": {
                name: {"state": check.state.value, "detail": check.detail}
                for name, check in sorted(self.checks.items())
            },
        }


class MetricsRegistry:
    """An instance-owned registry of metrics and health checks."""

    def __init__(self) -> None:
        self._counters: dict[str, Counter] = {}
        self._gauges: dict[str, Gauge] = {}
        self._histograms: dict[str, Histogram] = {}
        self._health: dict[str, HealthCheck] = {}

    def _reject_reuse(self, name: str, kind: MetricKind) -> None:
        for existing_kind, table in (
            (MetricKind.COUNTER, self._counters),
            (MetricKind.GAUGE, self._gauges),
            (MetricKind.HISTOGRAM, self._histograms),
        ):
            if name in table and existing_kind is not kind:
                raise MetricError(
                    f"metric {name!r} is already registered as a {existing_kind.value}; "
                    "one name must mean one thing or a dashboard reports two things at once"
                )

    def counter(self, name: str) -> Counter:
        self._reject_reuse(name, MetricKind.COUNTER)
        return self._counters.setdefault(name, Counter(name))

    def gauge(self, name: str) -> Gauge:
        self._reject_reuse(name, MetricKind.GAUGE)
        return self._gauges.setdefault(name, Gauge(name))

    def histogram(self, name: str, quantiles: Sequence[float] = DEFAULT_QUANTILES) -> Histogram:
        self._reject_reuse(name, MetricKind.HISTOGRAM)
        return self._histograms.setdefault(name, Histogram(name, quantiles))

    def register_health_check(self, name: str, state: HealthState = HealthState.HEALTHY,
                              detail: str = "") -> HealthCheck:
        check = HealthCheck(name=name, state=state, detail=detail)
        self._health[name] = check
        return check

    def set_health(self, name: str, state: HealthState, detail: str = "") -> None:
        if name not in self._health:
            raise MetricError(f"health check {name!r} was never registered")
        self._health[name] = HealthCheck(name=name, state=state, detail=detail)

    def health(self) -> HealthReport:
        return HealthReport(checks=dict(self._health))

    def snapshot(self) -> MetricsSnapshot:
        """Take an immutable copy. Observers never hold a live handle."""
        return MetricsSnapshot(
            schema_version=SNAPSHOT_SCHEMA_VERSION,
            counters={name: c.value for name, c in self._counters.items()},
            gauges={name: g.value for name, g in self._gauges.items()},
            histograms={name: h.snapshot() for name, h in self._histograms.items()},
        )

    @property
    def names(self) -> list[str]:
        return sorted([*self._counters, *self._gauges, *self._histograms])


class LatencyTimer:
    """Measure a span with a monotonic clock and record it in a histogram.

    Only monotonic readings are accepted, because docs/ARCHITECTURE.md forbids
    deriving latency from wall-clock stamps without documented synchronisation —
    and the type system already refuses to subtract across clock domains.
    """

    def __init__(self, histogram: Histogram, clock: SteadyClock) -> None:
        self._histogram = histogram
        self._clock = clock
        self._started: MonotonicTime | None = None

    def __enter__(self) -> LatencyTimer:
        self._started = self._clock.now()
        return self

    def __exit__(self, *exc_info: object) -> None:
        if self._started is None:  # pragma: no cover - __enter__ always sets it
            return
        elapsed_ns: Nanos = self._clock.now().nanos - self._started.nanos
        self._histogram.observe(float(elapsed_ns))
        self._started = None
