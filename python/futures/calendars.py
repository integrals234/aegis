"""Trading-session calendars and runtime classification (AEGIS-013).

Two layers, deliberately kept apart:

* :class:`CalendarTemplate` -- a human-authored, **local-time** definition
  (``configs/calendars/templates/*.yaml``): named session blocks with a local
  start/end time-of-day, the weekdays they start on, and an explicit holiday
  date list. Consumed only by ``tools/generate_calendars.py``.
* :class:`CalendarRegistry` -- loads the **generated, committed, normalized
  UTC-interval** artifact (``configs/calendars/generated/*.json``) and answers
  :meth:`CalendarRegistry.classify`. This is the only calendar surface any
  runtime caller -- ``python/futures/ingest.py``, ``roll``, ``series``, or a
  future participant book builder -- may use.

The split exists because runtime classification must be pure, deterministic
and independent of the host's timezone database: two machines with different
system tzdata versions must classify identically. ``zoneinfo`` therefore
appears **nowhere in this module** -- it is imported exactly once, by the
offline generator, which runs under the pinned ``tzdata`` package
(``requirements/python-requirements.in``) so its output is reproducible
regardless of which machine ran it. This module reads that committed output
and nothing else: no wall clock, no locale, no hash-order dependency.

``Product.session_template`` (``python/futures/instruments.py``) is the name
a :class:`CalendarRegistry` template key must match; nothing before this
module interprets that field.
"""

from __future__ import annotations

import bisect
import itertools
import json
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from datetime import date, time
from enum import StrEnum
from pathlib import Path
from typing import Any, Final

import jsonschema
import yaml

__all__ = [
    "GENERATED_DIR",
    "GENERATED_SCHEMA_PATH",
    "SCHEMA_VERSION",
    "TEMPLATE_DIR",
    "TEMPLATE_SCHEMA_PATH",
    "WEEKDAYS",
    "CalendarInterval",
    "CalendarRegistry",
    "CalendarTemplate",
    "Classification",
    "InvalidCalendar",
    "SessionBlockSpec",
    "SessionState",
    "SessionType",
    "load_calendar_registry",
    "load_template",
    "load_template_schema",
]

SCHEMA_VERSION: Final[int] = 1
TEMPLATE_SCHEMA_PATH: Final[str] = "configs/schemas/futures_calendar_template.v1.json"
GENERATED_SCHEMA_PATH: Final[str] = "configs/schemas/futures_calendar.v1.json"
TEMPLATE_DIR: Final[str] = "configs/calendars/templates"
GENERATED_DIR: Final[str] = "configs/calendars/generated"

WEEKDAYS: Final[tuple[str, ...]] = ("mon", "tue", "wed", "thu", "fri", "sat", "sun")


class InvalidCalendar(ValueError):
    """A calendar template or generated artifact failed validation.

    Raised rather than repaired: an overlapping or malformed calendar
    definition is a configuration error the loader must refuse, not
    silently reconcile (CLAUDE.md: never mark complete by papering over an
    inconsistency).
    """


class SessionType(StrEnum):
    """What kind of block an interval belongs to, before classification."""

    REGULAR = "regular"
    OVERNIGHT = "overnight"
    MAINTENANCE = "maintenance"


class SessionState(StrEnum):
    """The runtime classification of one instant: a session type, or CLOSED.

    A timestamp classifies into exactly one of these four values -- never
    two -- because :class:`CalendarRegistry` rejects any generated calendar
    whose intervals overlap.
    """

    REGULAR = "regular"
    OVERNIGHT = "overnight"
    MAINTENANCE = "maintenance"
    CLOSED = "closed"


# ---------------------------------------------------------------------------
# Template layer -- local time, offline-generator input only.
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class SessionBlockSpec:
    """One named local-time session block within a template.

    ``local_end < local_start`` means the block wraps past local midnight --
    it ends on the calendar day *after* the one it starts on. Equality is
    rejected outright: a zero-length or "the whole day, ambiguously" block is
    not a fact this schema can represent, so it does not try to guess which
    was meant.
    """

    name: str
    type: SessionType
    local_start: time
    local_end: time
    days_of_week: frozenset[str]

    def __post_init__(self) -> None:
        if not self.name:
            raise InvalidCalendar("block name must be non-empty")
        if self.local_end == self.local_start:
            raise InvalidCalendar(
                f"block {self.name!r}: local_end must differ from local_start "
                "(a zero- or 24-hour block is ambiguous, not inferred)"
            )
        if not self.days_of_week:
            raise InvalidCalendar(f"block {self.name!r}: days_of_week must be non-empty")
        unknown = self.days_of_week - set(WEEKDAYS)
        if unknown:
            raise InvalidCalendar(f"block {self.name!r}: unknown weekday(s) {sorted(unknown)}")

    @property
    def wraps_midnight(self) -> bool:
        return self.local_end < self.local_start


@dataclass(frozen=True, slots=True)
class CalendarTemplate:
    """A named, timezone-scoped set of session blocks plus full-day holidays.

    Holiday suppression is keyed by a block instance's own **start** date:
    a block that starts on a holiday is suppressed for that instance even if
    (being an overnight block) it runs into the following day. The day it
    runs into is not itself checked against the holiday list -- only the
    day it started on decides whether that specific instance exists.
    """

    name: str
    timezone: str
    blocks: tuple[SessionBlockSpec, ...]
    holidays: frozenset[date]

    def __post_init__(self) -> None:
        if not self.name:
            raise InvalidCalendar("template name must be non-empty")
        if not self.timezone:
            raise InvalidCalendar(f"template {self.name!r}: timezone must be non-empty")
        if not self.blocks:
            raise InvalidCalendar(f"template {self.name!r}: must have at least one block")
        names = [b.name for b in self.blocks]
        if len(names) != len(set(names)):
            raise InvalidCalendar(f"template {self.name!r}: block names must be unique, got {names}")


def _describe(error: jsonschema.ValidationError) -> str:
    location = ".".join(str(part) for part in error.absolute_path) or "(root)"
    return f"{location}: {error.message}"


def _validate_against_schema(document: Mapping[str, Any], schema: Mapping[str, Any], where: str) -> None:
    declared = document.get("schema_version")
    if declared is None:
        raise InvalidCalendar(f"{where}: schema_version is required")
    if declared != SCHEMA_VERSION:
        raise InvalidCalendar(
            f"{where}: schema_version {declared!r} is not supported by this build "
            f"(this build understands version {SCHEMA_VERSION})"
        )
    validator = jsonschema.Draft202012Validator(schema)
    problems = sorted(validator.iter_errors(document), key=lambda e: list(e.absolute_path))
    if problems:
        detail = "\n".join(f"  - {_describe(problem)}" for problem in problems)
        raise InvalidCalendar(f"{where} is invalid ({len(problems)} problem(s)):\n{detail}")


def load_template_schema(root: Path) -> dict[str, Any]:
    path = root / TEMPLATE_SCHEMA_PATH
    if not path.exists():
        raise InvalidCalendar(f"calendar template schema not found at {TEMPLATE_SCHEMA_PATH}")
    schema: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    return schema


def load_template(root: Path, path: str) -> CalendarTemplate:
    """Load and schema-validate a template, offline-tool use only."""
    template_path = root / path
    if not template_path.exists():
        raise InvalidCalendar(f"calendar template not found at {path}")

    document = yaml.safe_load(template_path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise InvalidCalendar(f"{path} must contain a mapping at the top level")

    schema = load_template_schema(root)
    _validate_against_schema(document, schema, path)

    blocks = tuple(
        SessionBlockSpec(
            name=entry["name"],
            type=SessionType(entry["type"]),
            local_start=time.fromisoformat(entry["local_start"]),
            local_end=time.fromisoformat(entry["local_end"]),
            days_of_week=frozenset(entry["days_of_week"]),
        )
        for entry in document["blocks"]
    )
    holidays = frozenset(date.fromisoformat(d) for d in document["holidays"])
    return CalendarTemplate(
        name=document["template"],
        timezone=document["timezone"],
        blocks=blocks,
        holidays=holidays,
    )


# ---------------------------------------------------------------------------
# Generated layer -- committed, normalized UTC intervals. Runtime reads this.
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class CalendarInterval:
    """One committed, half-open ``[start_ns, end_ns)`` UTC interval."""

    name: str
    type: SessionType
    start_ns: int
    end_ns: int

    def __post_init__(self) -> None:
        if not self.name:
            raise InvalidCalendar("interval name must be non-empty")
        if self.end_ns <= self.start_ns:
            raise InvalidCalendar(
                f"interval {self.name!r}: end_ns ({self.end_ns}) must be > start_ns ({self.start_ns})"
            )


@dataclass(frozen=True, slots=True)
class Classification:
    """The result of classifying one UTC instant against one template."""

    state: SessionState
    session_name: str | None


def load_generated_schema(root: Path) -> dict[str, Any]:
    path = root / GENERATED_SCHEMA_PATH
    if not path.exists():
        raise InvalidCalendar(f"generated calendar schema not found at {GENERATED_SCHEMA_PATH}")
    schema: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    return schema


def _validate_no_overlap(template: str, ordered: Sequence[CalendarInterval]) -> None:
    """Reject illegal overlaps and duplicate/contradictory intervals.

    A single check suffices for both: two intervals sharing a start (a
    literal duplicate, or two contradictory definitions of the same instant)
    always satisfy ``curr.start_ns < prev.end_ns`` too, because every interval
    has positive width (enforced by :class:`CalendarInterval`).
    """
    for prev, curr in itertools.pairwise(ordered):
        if curr.start_ns < prev.end_ns:
            raise InvalidCalendar(
                f"template {template!r}: interval {curr.name!r} "
                f"[{curr.start_ns}, {curr.end_ns}) overlaps {prev.name!r} "
                f"[{prev.start_ns}, {prev.end_ns})"
            )


class CalendarRegistry:
    """Every loaded template's sorted, non-overlapping UTC intervals.

    Construction validates each template independently -- a defect in one
    generated file never silently affects another.
    """

    def __init__(self, calendars: Mapping[str, Sequence[CalendarInterval]]) -> None:
        self._calendars: dict[str, tuple[CalendarInterval, ...]] = {}
        self._starts: dict[str, list[int]] = {}
        for template, intervals in calendars.items():
            ordered = tuple(sorted(intervals, key=lambda iv: iv.start_ns))
            _validate_no_overlap(template, ordered)
            self._calendars[template] = ordered
            self._starts[template] = [iv.start_ns for iv in ordered]

    def __contains__(self, template: str) -> bool:
        return template in self._calendars

    def __len__(self) -> int:
        return len(self._calendars)

    def templates(self) -> tuple[str, ...]:
        return tuple(sorted(self._calendars))

    def intervals(self, template: str) -> tuple[CalendarInterval, ...]:
        if template not in self._calendars:
            raise InvalidCalendar(f"unknown calendar template: {template!r}")
        return self._calendars[template]

    def classify(self, as_of_ns: int, template: str) -> Classification:
        """Classify one UTC instant. Pure: no wall clock, no I/O.

        Half-open interval semantics: ``as_of_ns == start_ns`` classifies
        into the session (exactly-open); ``as_of_ns == end_ns`` does not
        (exactly-close falls to the next interval, or CLOSED).
        """
        if not isinstance(as_of_ns, int) or isinstance(as_of_ns, bool):
            raise InvalidCalendar(f"as_of_ns must be an int, got {type(as_of_ns).__name__}")
        if template not in self._calendars:
            raise InvalidCalendar(f"unknown calendar template: {template!r}")

        starts = self._starts[template]
        idx = bisect.bisect_right(starts, as_of_ns) - 1
        if idx >= 0:
            interval = self._calendars[template][idx]
            if interval.start_ns <= as_of_ns < interval.end_ns:
                return Classification(SessionState(interval.type.value), interval.name)
        return Classification(SessionState.CLOSED, None)


def load_calendar_registry(root: Path, directory: str = GENERATED_DIR) -> CalendarRegistry:
    """Load every generated calendar under ``root / directory``."""
    gen_dir = root / directory
    if not gen_dir.exists():
        raise InvalidCalendar(f"generated calendar directory not found: {directory}")

    schema = load_generated_schema(root)
    calendars: dict[str, list[CalendarInterval]] = {}
    for path in sorted(gen_dir.glob("*.json")):
        document = json.loads(path.read_text(encoding="utf-8"))
        _validate_against_schema(document, schema, str(path.relative_to(root)))

        template = document["template"]
        if template in calendars:
            raise InvalidCalendar(
                f"duplicate template {template!r} across generated calendar files "
                f"(second occurrence: {path.relative_to(root)})"
            )
        calendars[template] = [
            CalendarInterval(
                name=entry["name"],
                type=SessionType(entry["type"]),
                start_ns=entry["start_ns"],
                end_ns=entry["end_ns"],
            )
            for entry in document["intervals"]
        ]
    if not calendars:
        raise InvalidCalendar(f"no generated calendars found under {directory}")
    return CalendarRegistry(calendars)
