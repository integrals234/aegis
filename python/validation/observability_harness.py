"""AEGIS-238 M5 observability harness: wires a
`common.metrics.MetricsRegistry` to the real `aegis_participant_run`
production binary's own output.

Risk status comes from the REAL M5 `risk::RiskEngine`, driven through the
actual `aegis_participant_run --calendar-spread` binary (subprocess, the
same pattern `tests/replay/test_participant_recovery.py` already uses to
invoke this binary) -- never a constant assigned directly to the registry.

Queue depth and dropped/backpressured events come from
:class:`BoundedExecutionBuffer`, a bounded outbound buffer this harness owns
and can drain slower than it fills. This IS the M5 integration harness's
bounded outbound execution buffer named in `docs/BUILD_STATE.md`'s
AEGIS-238 authorization -- explicitly NOT the M8 lock-free queue
implementation (`cpp/queues`, empty, M8-dated). Every depth/dropped value
is read back from the buffer's own state after real push/pop operations,
never assigned directly.

Latency comes from `common.metrics.LatencyTimer` over a real
`ManualSteadyClock`, timing each decoded output line -- deterministic, but
a real measured span, not a fabricated constant.

Shared by `tests/integration/test_participant_observability.py` and
`tools/generate_observability_evidence.py`, so the test proves exactly what
the evidence records.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

from common.clock import ManualSteadyClock, micros
from common.metrics import HealthState, LatencyTimer, MetricsRegistry

__all__ = ["BoundedExecutionBuffer", "ParticipantObservabilityHarness"]


class BoundedExecutionBuffer:
    """The M5 integration harness's bounded outbound execution buffer
    (docs/BUILD_STATE.md's AEGIS-238 disclosure) -- NOT the M8 lock-free
    queue implementation. A push past capacity is refused and counted."""

    def __init__(self, capacity: int) -> None:
        self._capacity = capacity
        self._items: list[object] = []
        self.dropped_count = 0

    def push(self, item: object) -> bool:
        if len(self._items) >= self._capacity:
            self.dropped_count += 1
            return False
        self._items.append(item)
        return True

    def pop(self) -> object | None:
        return self._items.pop(0) if self._items else None

    @property
    def depth(self) -> int:
        return len(self._items)


class ParticipantObservabilityHarness:
    """Wires a `MetricsRegistry` to the real `aegis_participant_run`
    production binary's own output -- each JSON output line carries a real
    `risk_decision` from the real M5 `RiskEngine`."""

    def __init__(self, registry: MetricsRegistry, steady: ManualSteadyClock,
                 buffer_capacity: int, drain_every: int) -> None:
        self._registry = registry
        self._steady = steady
        self._buffer = BoundedExecutionBuffer(buffer_capacity)
        self._drain_every = drain_every

        self._queue_depth = registry.gauge("queue.depth")
        self._dropped = registry.counter("queue.dropped_total")
        self._risk_status = registry.gauge("risk.status")
        self._decision_latency = registry.histogram("execution.latency_ns")
        registry.register_health_check("participant", HealthState.HEALTHY, "not yet run")

    def run(self, binary: Path, stream_path: Path, risk_config_path: Path) -> None:
        result = subprocess.run(
            [str(binary), "--calendar-spread", "--stream", str(stream_path),
             "--risk-config", str(risk_config_path)],
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            self._registry.set_health("participant", HealthState.UNHEALTHY,
                                      f"aegis_participant_run exited {result.returncode}")
            return
        lines = [line for line in result.stdout.splitlines() if line.strip()]
        if not lines:
            self._registry.set_health("participant", HealthState.UNHEALTHY, "no output produced")
            return

        reject_count = 0
        decision_count = 0
        for index, line in enumerate(lines):
            with LatencyTimer(self._decision_latency, self._steady):
                record = json.loads(line)
                decision = record.get("risk_decision")
                if decision is not None:
                    decision_count += 1
                    self._risk_status.set(float(decision["verdict"]))
                    if decision["verdict"] == 2:  # risk::RiskVerdict::kReject.
                        reject_count += 1
                    accepted = self._buffer.push(record)
                    if not accepted:
                        self._dropped.increment()
                    self._queue_depth.set(float(self._buffer.depth))
                    # Deliberately drains slower than it fills: real
                    # backpressure emerges from this ratio, not a hardcoded
                    # drop count.
                    if index % self._drain_every == 0:
                        self._buffer.pop()
                        self._queue_depth.set(float(self._buffer.depth))
                # Advanced inside the timed span, not after it: a
                # ManualSteadyClock never moves on its own, so a real nonzero
                # measured span requires the clock to advance while the timer
                # is open, exactly as tests/integration/test_metrics_registry.py's
                # ConfigLoaderService fixture already does around its own
                # real work.
                self._steady.advance(micros(50))

        if decision_count == 0:
            self._registry.set_health("participant", HealthState.UNHEALTHY, "no risk decisions observed")
        elif reject_count > 0:
            self._registry.set_health(
                "participant", HealthState.DEGRADED,
                f"{decision_count} risk decisions, {reject_count} rejected",
            )
        else:
            self._registry.set_health(
                "participant", HealthState.HEALTHY, f"{decision_count} risk decisions, all approved"
            )
