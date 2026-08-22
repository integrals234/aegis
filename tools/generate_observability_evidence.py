#!/usr/bin/env python3
"""Generate AEGIS-238 evidence: a full M5 observability attempt with real
producers, run once through the same harness
`tests/integration/test_participant_observability.py` exercises.

Explicitly discloses that queue depth / dropped events come from the M5
integration harness's bounded outbound execution buffer, NOT the M8
lock-free queue implementation (owner authorization,
docs/BUILD_STATE.md's AEGIS-238 section).

Regenerate with: python3 tools/generate_observability_evidence.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from common.clock import ManualSteadyClock
from common.determinism import resolve_participant_run_binary
from common.metrics import MetricsRegistry
from evidence_provenance import provenance
from validation.observability_harness import ParticipantObservabilityHarness


def main() -> int:
    binary = resolve_participant_run_binary(ROOT)
    registry = MetricsRegistry()
    steady = ManualSteadyClock()
    harness = ParticipantObservabilityHarness(registry, steady, buffer_capacity=1, drain_every=1_000_000)

    stream_path = ROOT / "tests/unit/fixtures/participant/calendar_spread_stream.jsonl"
    risk_config_path = ROOT / "configs/risk/limits_reject_demo.json"
    harness.run(binary, stream_path, risk_config_path)

    snapshot = registry.snapshot()
    health = registry.health()

    payload: dict[str, Any] = {
        **provenance(),
        "artifact": "participant_observability",
        "requirements": ["AEGIS-238"],
        "stream": str(stream_path.relative_to(ROOT)),
        "risk_config": str(risk_config_path.relative_to(ROOT)),
        "health": health.to_record(),
        "metrics": snapshot.to_record(),
        "queue_depth_and_dropped_events_disclosure": (
            "queue.depth and queue.dropped_total are produced by the M5 integration "
            "harness's bounded outbound execution buffer "
            "(python/validation/observability_harness.py's BoundedExecutionBuffer). "
            "THIS IS the harness's own bounded buffer. THIS IS NOT the M8 lock-free "
            "queue implementation (cpp/queues, empty and M8-dated) -- "
            "docs/BUILD_STATE.md's AEGIS-238 owner authorization requires this "
            "disclosure explicitly, and the fallback re-deferral of this residual "
            "portion to M8 is a closure-time decision this generator does not make."
        ),
        "risk_status_disclosure": (
            "risk.status is set from the real M5 risk::RiskEngine's own verdict, decoded "
            "from aegis_participant_run's real JSON output (subprocess), not fabricated."
        ),
        "claim": (
            "AEGIS-238: health, queue depth, dropped/backpressured events, latency and "
            f"risk status are all non-vacuous in this run. health.state={health.state.value}, "
            f"queue.dropped_total={snapshot.counters.get('queue.dropped_total', 0)}, "
            f"execution.latency_ns.count={snapshot.histograms['execution.latency_ns'].count}, "
            f"risk.status={snapshot.gauges.get('risk.status')}."
        ),
    }

    out_dir = ROOT / "experiments" / "evidence" / "AEGIS-238"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "participant_observability.json"
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
