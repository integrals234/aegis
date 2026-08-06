"""AEGIS-238 — metrics and health observed through a wired-up component.

Integration rather than unit: the acceptance criterion is "integration fixture
verifies metrics", and a registry tested in isolation proves only that a counter
counts. What matters is that a component with real producers reports state an
operator can act on, that the reported latency comes from a monotonic clock, and
that an observer cannot mutate what it observes.

The fixture below is a stand-in with real producers — config loads, log
emissions, harness timings. It is not a stub of a later subsystem: queue depth,
execution latency and risk status are absent here precisely because nothing
produces them yet (M1, M3 and M5 respectively).
"""

from __future__ import annotations

import pytest
from common.clock import ManualClock, ManualSteadyClock, micros
from common.config import ConfigError, resolve
from common.logging import ListSink, StructuredLogger
from common.metrics import (
    HealthState,
    LatencyTimer,
    MetricError,
    MetricsRegistry,
)

pytestmark = pytest.mark.integration


class ConfigLoaderService:
    """A component wired to a registry, a logger and two injected clocks.

    Everything it reports has a producer inside it. That is the rule this
    fixture exists to demonstrate: a metric appears when something feeds it.
    """

    def __init__(self, repo_root, registry: MetricsRegistry, logger: StructuredLogger,
                 steady: ManualSteadyClock) -> None:
        self._root = repo_root
        self._registry = registry
        self._logger = logger
        self._steady = steady

        self._loads = registry.counter("config.loads_total")
        self._failures = registry.counter("config.load_failures_total")
        self._records = registry.counter("log.records_emitted_total")
        self._version = registry.gauge("config.active_version")
        self._load_latency = registry.histogram("config.load_duration_ns")
        registry.register_health_check("config", HealthState.HEALTHY, "no configuration loaded yet")

    def load(self, path) -> bool:
        with LatencyTimer(self._load_latency, self._steady):
            try:
                resolved = resolve(self._root, path=path, environ={}, defaults={})
            except ConfigError as error:
                self._failures.increment()
                self._registry.set_health("config", HealthState.UNHEALTHY, str(error).splitlines()[0])
                self._logger.error("configuration rejected", path=str(path))
                self._records.increment()
                return False

        self._loads.increment()
        self._version.set(float(resolved.config_version))
        self._registry.set_health("config", HealthState.HEALTHY, f"loaded {path.name}")
        self._logger.info("configuration loaded", experiment=resolved.experiment_id)
        self._records.increment()
        return True


@pytest.fixture
def steady():
    return ManualSteadyClock()


@pytest.fixture
def service(repo_root, steady):
    registry = MetricsRegistry()
    sink = ListSink()
    logger = StructuredLogger("platform.config", "m0-metrics", ManualClock(1_000), sink)
    return ConfigLoaderService(repo_root, registry, logger, steady), registry, sink


CORPUS = "tests/unit/fixtures/configs"


def test_successful_load_moves_every_metric_it_should(service, repo_root, steady):
    component, registry, _ = service
    steady.advance(micros(0))
    assert component.load(repo_root / f"{CORPUS}/valid/minimal.json")

    snapshot = registry.snapshot()
    assert snapshot.counters["config.loads_total"] == 1
    assert snapshot.counters["config.load_failures_total"] == 0
    assert snapshot.counters["log.records_emitted_total"] == 1
    assert snapshot.gauges["config.active_version"] == 1.0
    assert snapshot.histograms["config.load_duration_ns"].count == 1


def test_failed_load_is_visible_as_unhealthy_with_a_reason(service, repo_root):
    component, registry, _ = service
    assert not component.load(repo_root / f"{CORPUS}/invalid/future_config_version.json")

    report = registry.health()
    assert report.state is HealthState.UNHEALTHY
    assert report.checks["config"].detail, "a health state without a reason is not actionable"
    assert registry.snapshot().counters["config.load_failures_total"] == 1


def test_health_recovers_when_the_next_load_succeeds(service, repo_root):
    component, registry, _ = service
    component.load(repo_root / f"{CORPUS}/invalid/negative_seed.json")
    assert registry.health().state is HealthState.UNHEALTHY

    component.load(repo_root / f"{CORPUS}/valid/full.json")
    assert registry.health().state is HealthState.HEALTHY


def test_overall_health_is_the_worst_check(service):
    _, registry, _ = service
    registry.register_health_check("feed", HealthState.HEALTHY)
    registry.register_health_check("gateway", HealthState.DEGRADED, "reconnecting")

    assert registry.health().state is HealthState.DEGRADED

    registry.set_health("feed", HealthState.UNHEALTHY, "disconnected")
    assert registry.health().state is HealthState.UNHEALTHY, (
        "a component is only as healthy as its unhealthiest part"
    )


def test_latency_is_measured_with_the_monotonic_clock(service):
    """A wall clock can step; a latency derived from one can come out negative."""
    _, registry, _ = service

    class TickingClock(ManualSteadyClock):
        def now(self):
            reading = super().now()
            self.advance(micros(25))
            return reading

    ticking = TickingClock()
    histogram = registry.histogram("probe_duration_ns")
    with LatencyTimer(histogram, ticking):
        pass

    assert histogram.snapshot().count == 1
    assert histogram.snapshot().minimum == 25_000.0


def test_snapshot_is_an_immutable_copy(service, repo_root):
    """An observer must not be able to mutate what it observes."""
    component, registry, _ = service
    component.load(repo_root / f"{CORPUS}/valid/minimal.json")

    snapshot = registry.snapshot()
    before = snapshot.counters["config.loads_total"]
    component.load(repo_root / f"{CORPUS}/valid/full.json")

    assert snapshot.counters["config.loads_total"] == before
    assert registry.snapshot().counters["config.loads_total"] == before + 1


def test_snapshot_record_is_deterministic(service, repo_root):
    """The record is written to disk and hashed, so key order must be stable."""
    component, registry, _ = service
    component.load(repo_root / f"{CORPUS}/valid/minimal.json")
    assert registry.snapshot().to_record() == registry.snapshot().to_record()


def test_histogram_reports_the_tail_not_only_the_mean(service):
    """docs/BENCHMARK_POLICY.md forbids quoting mean latency alone."""
    _, registry, _ = service
    histogram = registry.histogram("probe_ns")
    for value in range(1, 1001):
        histogram.observe(float(value))

    snapshot = histogram.snapshot()
    assert set(snapshot.quantiles) == {"p50", "p95", "p99", "p99.9"}
    assert snapshot.quantiles["p50"] == 500.0
    # Nearest rank: ceil(0.999 * 1000) = 999, so p99.9 is the 999th value.
    assert snapshot.quantiles["p99.9"] == 999.0
    assert snapshot.maximum == 1000.0


def test_quantiles_are_nearest_rank_not_interpolated(service):
    """An interpolated p99.9 reports a latency no request actually experienced."""
    _, registry, _ = service
    histogram = registry.histogram("probe_ns")
    for value in (1.0, 2.0, 100.0):
        histogram.observe(value)
    assert histogram.snapshot().quantiles["p99"] in (1.0, 2.0, 100.0)


def test_counter_cannot_decrease(service):
    """A counter that can go down makes every rate derived from it wrong."""
    _, registry, _ = service
    with pytest.raises(MetricError, match="cannot decrease"):
        registry.counter("c").increment(-1)


def test_a_name_cannot_change_kind(service):
    _, registry, _ = service
    registry.counter("shared.name")
    with pytest.raises(MetricError, match="already registered as a counter"):
        registry.gauge("shared.name")


def test_unregistered_health_check_cannot_be_set(service):
    _, registry, _ = service
    with pytest.raises(MetricError, match="never registered"):
        registry.set_health("never-declared", HealthState.UNHEALTHY)


def test_no_domain_metric_is_pre_registered(service, repo_root):
    """A gauge nobody writes reads zero, and an operator believes the zero.

    Queue depth, execution latency and risk status arrive with their producers
    in M1, M3 and M5. Registering them now would make M0 look observable.
    """
    component, registry, _ = service
    component.load(repo_root / f"{CORPUS}/valid/minimal.json")

    for absent in ("queue.depth", "queue.dropped_total", "execution.latency_ns", "risk.status"):
        assert absent not in registry.names
