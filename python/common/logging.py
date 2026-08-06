"""Structured JSON Lines logging (AEGIS-232).

Four properties, each answering a specific way logs stop being useful:

**Machine-readable.** One JSON object per line, validated against
``configs/schemas/log_record.v1.json``. Free-text logs cannot be joined,
filtered or diffed, so they cannot serve as evidence.

**Correlated.** Every record carries ``experiment_id`` — the same field the
message envelope carries — plus an optional ``correlation_id`` for one causal
chain. Without them a log line and the event that produced it can only be
matched by timestamp and hope.

**Deterministic.** The clock is injected and a per-logger sequence number breaks
timestamp ties, so a fixture run twice produces byte-identical output. That is
what lets a log file be an input to the determinism harness rather than a source
of spurious diffs.

**Secret-free.** Field values are redacted by key name before serialization.
A logger is the most common way a credential reaches disk, and the check has to
live where the record is built rather than in a review convention.

The logger is an instance. There is no module-level default and no
``get_logger()`` singleton: a process-global logger is shared mutable state
reachable from every book partition, which is what AEGIS-047 exists to prevent.
"""

from __future__ import annotations

import json
import re
from collections.abc import Mapping
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import Any, Protocol, TextIO

from common.clock import Nanos, WallClock

SCHEMA_VERSION = 1
SCHEMA_PATH = "configs/schemas/log_record.v1.json"

REDACTED = "[redacted]"

# Field names whose values never reach a log record. Matched case-insensitively
# against the whole key, so `api_key`, `AWS_SECRET` and `db.password` all hit.
SECRET_KEY_PATTERN = re.compile(
    r"(?i)(password|passwd|secret|token|api[_-]?key|access[_-]?key|credential|private[_-]?key|authorization)"
)


class Level(IntEnum):
    """Ordered so a threshold comparison is a single integer test."""

    TRACE = 10
    DEBUG = 20
    INFO = 30
    WARN = 40
    ERROR = 50

    @property
    def label(self) -> str:
        return self.name.lower()

    @classmethod
    def parse(cls, name: str) -> Level:
        try:
            return cls[name.strip().upper()]
        except KeyError:
            valid = ", ".join(level.label for level in cls)
            raise ValueError(f"unknown log level {name!r}; expected one of {valid}") from None


class Sink(Protocol):
    """Where records go. A sink writes one line and does not interpret it."""

    def write_line(self, line: str) -> None: ...


class StreamSink:
    """Write records to an open text stream."""

    def __init__(self, stream: TextIO) -> None:
        self._stream = stream

    def write_line(self, line: str) -> None:
        self._stream.write(line + "\n")
        self._stream.flush()


class ListSink:
    """Collect records in memory. The sink tests and fixtures use."""

    def __init__(self) -> None:
        self.lines: list[str] = []

    def write_line(self, line: str) -> None:
        self.lines.append(line)

    @property
    def records(self) -> list[dict[str, Any]]:
        return [json.loads(line) for line in self.lines]


class FileSink:
    """Append records to a file, creating parent directories as needed."""

    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._path = path

    def write_line(self, line: str) -> None:
        with self._path.open("a", encoding="utf-8") as handle:
            handle.write(line + "\n")


def redact(fields: Mapping[str, Any]) -> dict[str, Any]:
    """Replace secret-looking values, keeping the key so the shape stays readable."""
    return {
        key: (REDACTED if SECRET_KEY_PATTERN.search(key) else value) for key, value in fields.items()
    }


def _coerce_value(value: Any) -> str | int | float | bool | None:
    """Force a field value into a scalar the schema permits.

    Anything structured is rendered to its repr rather than nested. Unbounded
    nested payloads on a hot path are how a log becomes the slowest part of a
    system, and a record whose shape varies per call cannot be queried.
    """
    if value is None or isinstance(value, str | int | float | bool):
        return value
    return repr(value)


@dataclass(frozen=True)
class LogRecord:
    """One structured record, in the field order the schema declares."""

    schema_version: int
    timestamp_ns: Nanos
    level: str
    logger: str
    message: str
    experiment_id: str
    sequence: int
    correlation_id: str | None = None
    fields: Mapping[str, Any] | None = None

    def to_json(self) -> str:
        """Serialize with a fixed key order, so two identical runs produce identical bytes."""
        payload: dict[str, Any] = {
            "schema_version": self.schema_version,
            "timestamp_ns": self.timestamp_ns,
            "level": self.level,
            "logger": self.logger,
            "message": self.message,
            "experiment_id": self.experiment_id,
            "sequence": self.sequence,
        }
        if self.correlation_id is not None:
            payload["correlation_id"] = self.correlation_id
        if self.fields:
            payload["fields"] = dict(sorted(self.fields.items()))
        return json.dumps(payload, separators=(",", ":"), ensure_ascii=False)


class StructuredLogger:
    """A logger instance bound to one experiment, clock and sink."""

    def __init__(
        self,
        name: str,
        experiment_id: str,
        clock: WallClock,
        sink: Sink,
        level: Level = Level.INFO,
        correlation_id: str | None = None,
    ) -> None:
        if not name:
            raise ValueError("logger name is required; records must say what emitted them")
        if not experiment_id:
            raise ValueError(
                "experiment_id is required; a record that cannot be joined to its run "
                "is not evidence of anything"
            )
        self._name = name
        self._experiment_id = experiment_id
        self._clock = clock
        self._sink = sink
        self._level = level
        self._correlation_id = correlation_id
        self._sequence = 0

    @property
    def sequence(self) -> int:
        return self._sequence

    @property
    def level(self) -> Level:
        return self._level

    def bind(self, *, name: str | None = None, correlation_id: str | None = None) -> StructuredLogger:
        """Derive a child logger sharing this logger's clock, sink and experiment.

        The child keeps its own sequence counter: sequence numbers order the
        records of one emitter, and sharing a counter across components would
        make the ordering depend on interleaving.
        """
        return StructuredLogger(
            name=name or self._name,
            experiment_id=self._experiment_id,
            clock=self._clock,
            sink=self._sink,
            level=self._level,
            correlation_id=correlation_id if correlation_id is not None else self._correlation_id,
        )

    def log(self, level: Level, message: str, **fields: Any) -> LogRecord | None:
        if level < self._level:
            return None
        record = LogRecord(
            schema_version=SCHEMA_VERSION,
            timestamp_ns=self._clock.now_utc(),
            level=level.label,
            logger=self._name,
            message=message,
            experiment_id=self._experiment_id,
            sequence=self._sequence,
            correlation_id=self._correlation_id,
            fields={key: _coerce_value(value) for key, value in redact(fields).items()} or None,
        )
        self._sequence += 1
        self._sink.write_line(record.to_json())
        return record

    def trace(self, message: str, **fields: Any) -> LogRecord | None:
        return self.log(Level.TRACE, message, **fields)

    def debug(self, message: str, **fields: Any) -> LogRecord | None:
        return self.log(Level.DEBUG, message, **fields)

    def info(self, message: str, **fields: Any) -> LogRecord | None:
        return self.log(Level.INFO, message, **fields)

    def warn(self, message: str, **fields: Any) -> LogRecord | None:
        return self.log(Level.WARN, message, **fields)

    def error(self, message: str, **fields: Any) -> LogRecord | None:
        return self.log(Level.ERROR, message, **fields)


def load_schema(root: Path) -> dict[str, Any]:
    schema: dict[str, Any] = json.loads((root / SCHEMA_PATH).read_text(encoding="utf-8"))
    return schema
