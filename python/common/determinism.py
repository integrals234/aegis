"""Canonical-output producers for the determinism harness (AEGIS-005).

AEGIS-005 requires that identical event input and seed produce byte-identical
canonical output before any performance work begins. M0 has no engine, so what
is proved here is narrower and stated plainly:

    **the harness detects nondeterminism.**

Not "AEGIS is deterministic" — there is nothing yet whose determinism could be
claimed. The producers below emit *platform* records: a resolved configuration,
a metrics snapshot, and a stream of message envelopes with opaque payloads. No
order, trade, book or sequencer type appears, because none exists.

Two producers are registered, and the second is the point of the file. A harness
that has only ever seen deterministic output cannot demonstrate that it would
notice nondeterministic output; ``nondeterministic`` exists so that claim is
tested rather than assumed.
"""

from __future__ import annotations

import json
import random
from collections.abc import Callable, Mapping
from pathlib import Path

from common.clock import EventTime, ManualClock, millis
from common.envelope import Envelope, MessageType, encode
from common.logging import ListSink, StructuredLogger
from common.metrics import MetricsRegistry

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


PRODUCERS: dict[str, Callable[[int, Path | None], str]] = {
    "platform": platform_producer,
    "nondeterministic": nondeterministic_producer,
}


def run_producer(name: str, seed: int, root: Path | None = None) -> str:
    try:
        producer = PRODUCERS[name]
    except KeyError:
        known = ", ".join(sorted(PRODUCERS))
        raise KeyError(f"unknown producer {name!r}; registered producers are {known}") from None
    return producer(seed, root)
