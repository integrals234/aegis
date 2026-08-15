"""Canonical-output producers for the determinism harness (AEGIS-005).

AEGIS-005 requires that identical event input and seed produce byte-identical
canonical output before any performance work begins. M0 has no engine, so what
is proved here is narrower and stated plainly:

    **the harness detects nondeterminism.**

Not "AEGIS is deterministic" — at M0 there was nothing yet whose determinism
could be claimed, and ``platform_producer``/``nondeterministic_producer``
still emit only platform records: a resolved configuration, a metrics
snapshot, a stream of message envelopes with opaque payloads. ``nondeterministic``
is the negative fixture: a harness that has only ever seen deterministic
output cannot demonstrate that it would notice nondeterministic output.

``exchange_producer`` (M1, ADR-0012) is the point at which this stops being
narrow: it runs the actual matching engine (via ``aegis_exchange_replay``)
and its canonical output is the AEGIS-005 discharge for the exchange core.

``futures_replay_producer`` (M2 slice 14) does the same job for the *replay*
core: it runs ``aegis_replay_run`` over the committed canonical stream, so
AEGIS-058's "repeated runs produce identical outputs" is shown across separate
OS processes rather than across two calls inside one test binary. The
distinction is the whole reason this module spawns subprocesses at all, and it
applies to the replay core exactly as it does to the exchange.
"""

from __future__ import annotations

import json
import os
import random
import subprocess
from collections.abc import Callable, Mapping
from pathlib import Path

from common.clock import EventTime, ManualClock, millis
from common.envelope import Envelope, MessageType, encode
from common.logging import ListSink, StructuredLogger
from common.metrics import MetricsRegistry

# The committed scenario aegis_exchange_replay runs (tests/cpp/unit's own
# copy exercises the same commands at the unit-test level, in-process;
# tests/cpp/unit/test_snapshot_roundtrip.cpp's ContinuationEqualityAcross...
# case builds its command list to match).
EXCHANGE_SCENARIO_RELATIVE_PATH = "tests/unit/fixtures/exchange/replay/basic_session.jsonl"

# The committed canonical replay stream aegis_replay_run replays (M2 slice 14).
# Derived from the committed bar samples by tools/make_replay_fixture.py using
# the real ingestion pipeline; tests/replay/test_futures_replay_determinism.py
# fails if the committed bytes ever drift from what that pipeline produces.
# It is referenced by path rather than rebuilt here because this module lives in
# layer python-common, which may not depend on python-futures
# (configs/architecture_rules.yaml).
FUTURES_REPLAY_STREAM_RELATIVE_PATH = "tests/unit/fixtures/replay/futures_canonical_stream.jsonl"

# A stable logical name for the stream, used instead of the filesystem path in
# the emitted manifest. Two runs over the same content must agree, and an
# absolute path would make the output depend on where the repository is checked
# out rather than on what was replayed.
FUTURES_REPLAY_STREAM_LABEL = "futures_canonical_stream.v1"

CANONICAL_FORMAT_VERSION = 1

# A fixed base time, so output depends on the seed and nothing else. Reading the
# real clock here would make every run differ and the harness would report
# nondeterminism it had itself introduced.
BASE_EVENT_NANOS = 1_700_000_000_000_000_000


def _canonical_document(records: list[Mapping[str, object]]) -> str:
    """Serialize with sorted keys and no insignificant whitespace, one line each.

    Sorted keys rather than insertion order: Python dict order is insertion
    order, which depends on code paths, and the whole point of a canonical form
    is that it does not.
    """
    lines = [f'{{"canonical_format_version":{CANONICAL_FORMAT_VERSION}}}']
    lines += [json.dumps(record, sort_keys=True, separators=(",", ":")) for record in records]
    return "\n".join(lines) + "\n"


def platform_producer(seed: int, root: Path | None = None) -> str:
    """Emit canonical output for the M0 platform substrate.

    Everything time-dependent comes from a ManualClock and everything random
    from a seeded generator, so the only inputs are the seed and the committed
    fixtures.
    """
    rng = random.Random(seed)
    clock = ManualClock(BASE_EVENT_NANOS)
    registry = MetricsRegistry()
    sink = ListSink()
    logger = StructuredLogger("determinism.producer", f"det-{seed}", clock, sink)

    messages = registry.counter("producer.messages_total")
    payload_bytes = registry.counter("producer.payload_bytes_total")
    spread = registry.histogram("producer.payload_size")

    records: list[Mapping[str, object]] = []
    for sequence in range(64):
        size = rng.randrange(0, 24)
        payload = bytes(rng.randrange(0, 256) for _ in range(size))
        envelope = Envelope(
            sequence=sequence,
            stream_id=rng.randrange(0, 4),
            event_time=EventTime(clock.now_utc()),
            producer_id="determinism.producer",
            experiment_id=f"det-{seed}",
            correlation_id=f"chain-{sequence % 5}",
            payload=payload,
            message_type=MessageType.UNSPECIFIED,
        )
        records.append({"kind": "envelope", "hex": encode(envelope).hex()})

        messages.increment()
        payload_bytes.increment(size)
        spread.observe(float(size))
        logger.info("emitted", sequence=sequence, size=size)
        clock.advance(millis(1))

    records.append({"kind": "metrics", "snapshot": registry.snapshot().to_record()})
    records.append({"kind": "log", "lines": sink.lines})
    if root is not None:
        from common.config import resolve

        resolved = resolve(
            root,
            path=root / "tests/unit/fixtures/configs/valid/full.json",
            environ={},
            defaults={},
        )
        records.append({"kind": "config", "digest": resolved.digest()})
    return _canonical_document(records)


def nondeterministic_producer(seed: int, root: Path | None = None) -> str:
    """A producer that is deliberately not reproducible. The negative fixture.

    It uses an unseeded generator, which is the most common way real
    nondeterminism enters a research pipeline: somebody reaches for the module
    level ``random`` instead of the injected one. The harness must flag this run
    as failing. If it ever passes, the harness is not measuring what it claims.
    """
    del seed, root
    records: list[Mapping[str, object]] = [
        {"kind": "unseeded", "value": random.random()} for _ in range(4)
    ]
    return _canonical_document(records)


def resolve_exchange_replay_binary(root: Path) -> Path:
    """Locate ``aegis_exchange_replay``: ``AEGIS_EXCHANGE_REPLAY`` if set, else the
    debug preset's default build output path.

    A missing binary raises, never skips (ADR-0012) -- a skipping determinism
    test is a green stub that proves nothing.
    """
    env_path = os.environ.get("AEGIS_EXCHANGE_REPLAY")
    binary = Path(env_path) if env_path else root / "build/debug/cpp/exchange/app/aegis_exchange_replay"
    if not binary.exists():
        raise FileNotFoundError(
            f"aegis_exchange_replay not found at {binary}. Build it with "
            "'cmake --build --preset debug', or set AEGIS_EXCHANGE_REPLAY to its path."
        )
    return binary


def resolve_participant_run_binary(root: Path) -> Path:
    """Locate ``aegis_participant_run``: ``AEGIS_PARTICIPANT_RUN`` if set, else
    the debug preset's default build output path.

    A missing binary raises, never skips -- same rule as
    :func:`resolve_exchange_replay_binary`, for the same reason (AEGIS-237's
    process-boundary recovery proof is worthless if it silently skips).
    """
    env_path = os.environ.get("AEGIS_PARTICIPANT_RUN")
    binary = (
        Path(env_path) if env_path else root / "build/debug/cpp/participant/app/aegis_participant_run"
    )
    if not binary.exists():
        raise FileNotFoundError(
            f"aegis_participant_run not found at {binary}. Build it with "
            "'cmake --build --preset debug', or set AEGIS_PARTICIPANT_RUN to its path."
        )
    return binary


def exchange_producer(seed: int, root: Path | None = None) -> str:
    """Run ``aegis_exchange_replay`` on the committed M1 scenario and return its
    canonical stdout (ADR-0012, ADR-0013) -- the AEGIS-005 discharge for the
    exchange core.

    The matching engine reads no clock and no random source (ADR-0012;
    ``tests/unit/test_exchange_determinism_lint.py`` enforces this), so unlike
    ``platform_producer`` there is nothing for ``seed`` to seed. It is accepted
    only to satisfy the ``Callable[[int, Path | None], str]`` shape every
    registered producer shares, and is otherwise unused.
    """
    del seed
    if root is None:
        raise ValueError("exchange_producer requires root to locate the replay binary and fixture")
    binary = resolve_exchange_replay_binary(root)
    fixture = root / EXCHANGE_SCENARIO_RELATIVE_PATH
    if not fixture.exists():
        raise FileNotFoundError(f"exchange scenario fixture not found: {fixture}")
    result = subprocess.run(
        [str(binary), "--input", str(fixture)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"aegis_exchange_replay failed (exit {result.returncode}): {result.stderr}")
    return result.stdout


def resolve_replay_run_binary(root: Path) -> Path:
    """Locate ``aegis_replay_run``: ``AEGIS_REPLAY_RUN`` if set, else the debug
    preset's default build output path.

    A missing binary raises, never skips -- same rule as
    :func:`resolve_exchange_replay_binary`, for the same reason: a determinism
    check that skips is a green stub.
    """
    env_path = os.environ.get("AEGIS_REPLAY_RUN")
    binary = Path(env_path) if env_path else root / "build/debug/cpp/replay/aegis_replay_run"
    if not binary.exists():
        raise FileNotFoundError(
            f"aegis_replay_run not found at {binary}. Build it with "
            "'cmake --build --preset debug', or set AEGIS_REPLAY_RUN to its path."
        )
    return binary


def futures_replay_producer(seed: int, root: Path | None = None) -> str:
    """Run ``aegis_replay_run`` over the committed canonical stream and return
    its canonical stdout -- the cross-process half of AEGIS-058.

    The run deliberately exercises more than a bare drain: accelerated pacing
    (AEGIS-055) so a computed wait appears on every line, and two committed
    fault rules (AEGIS-060/061) so an annotation and an accounted-for drop are
    both in the compared output. Anything nondeterministic in ordering, pacing
    arithmetic or fault application therefore surfaces as a byte difference.

    ``seed`` is unused: replay reads no random source, and the fault rules are
    explicit committed data rather than samples (ADR-0019). It is accepted only
    to satisfy the shape every registered producer shares.
    """
    del seed
    if root is None:
        raise ValueError("futures_replay_producer requires root to locate the binary and fixture")
    binary = resolve_replay_run_binary(root)
    fixture = root / FUTURES_REPLAY_STREAM_RELATIVE_PATH
    if not fixture.exists():
        raise FileNotFoundError(
            f"canonical replay stream fixture not found: {fixture}. "
            "Regenerate it with 'python3 tools/make_replay_fixture.py'."
        )
    result = subprocess.run(
        [
            str(binary),
            "--input", str(fixture),
            "--label", FUTURES_REPLAY_STREAM_LABEL,
            "--pacing", "accelerated",
            "--multiplier", "2",
            "--fault", "3:delayed:500",
            "--fault", "5:missing",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"aegis_replay_run failed (exit {result.returncode}): {result.stderr}")
    return result.stdout


PRODUCERS: dict[str, Callable[[int, Path | None], str]] = {
    "platform": platform_producer,
    "nondeterministic": nondeterministic_producer,
    "exchange": exchange_producer,
    "futures_replay": futures_replay_producer,
}


def run_producer(name: str, seed: int, root: Path | None = None) -> str:
    try:
        producer = PRODUCERS[name]
    except KeyError:
        known = ", ".join(sorted(PRODUCERS))
        raise KeyError(f"unknown producer {name!r}; registered producers are {known}") from None
    return producer(seed, root)
