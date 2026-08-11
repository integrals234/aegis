#!/usr/bin/env python3
"""Offline, deterministic UTC-interval calendar generator (AEGIS-013).

This is the **only** code path in AEGIS that imports ``zoneinfo`` for futures
session calendars. ``python/futures/calendars.py``, which every runtime
caller uses, never touches ``zoneinfo`` or the host tz database -- it reads
exactly the committed output of this tool
(``configs/calendars/generated/*.json``). Pinned ``tzdata``
(``requirements/python-requirements.in``) makes that output reproducible on
any machine, independent of the host's own tz database version -- which is
exactly why runtime classification is not allowed to call ``zoneinfo``
directly (two machines with different system tzdata would then classify
differently).

Regenerate with: ``python3 tools/generate_calendars.py``. Output is
deterministic: re-running against the same templates and window reproduces
byte-identical JSON.

The generation window (``WINDOW_START``..``WINDOW_END``, exclusive) is fixed
and committed rather than "today plus N years" -- a wall-clock-relative
window would make the generated artifact change on every regeneration for no
configuration reason, which is exactly the kind of hidden nondeterminism
CLAUDE.md prohibits. The window covers the Slice 2 contract fixtures
(2026-2027) with a year of margin on each side, and both 2026 US DST
transitions (2026-03-08 spring-forward, 2026-11-01 fall-back), which
``tests/unit/test_futures_calendars.py`` and
``tests/property/test_session_classification.py`` assert against directly.
"""

from __future__ import annotations

import itertools
import json
import sys
from datetime import UTC, date, datetime, time, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from futures.calendars import (
    GENERATED_DIR,
    SCHEMA_VERSION,
    TEMPLATE_DIR,
    CalendarTemplate,
    SessionBlockSpec,
    load_template,
)

WINDOW_START = date(2025, 1, 1)
WINDOW_END = date(2028, 1, 1)  # exclusive

_WEEKDAY_NAMES = ("mon", "tue", "wed", "thu", "fri", "sat", "sun")
_EPOCH = datetime(1970, 1, 1, tzinfo=UTC)


def _local_datetime(day: date, local_time: time, tz: ZoneInfo) -> datetime:
    return datetime.combine(day, local_time, tzinfo=tz)


def _to_ns(dt: datetime) -> int:
    """Exact nanoseconds since the epoch. No float arithmetic.

    ``timedelta`` subtraction of two timezone-aware datetimes is exact
    (``days``/``seconds``/``microseconds`` are integers); multiplying an
    epoch-seconds float by 1e9 would silently lose precision at nanosecond
    scale, which is exactly the kind of thing a deterministic-replay platform
    cannot tolerate in a committed artifact.
    """
    delta = dt.astimezone(UTC) - _EPOCH
    if delta.microseconds:
        raise SystemExit(f"unexpected sub-second component in generated instant: {dt!r}")
    seconds = delta.days * 86400 + delta.seconds
    return seconds * 1_000_000_000


def expand_block(block: SessionBlockSpec, tz: ZoneInfo, start: date, end: date) -> list[dict[str, object]]:
    """Every instance of ``block`` in ``[start, end)``, before holiday filtering."""
    intervals: list[dict[str, object]] = []
    day = start
    one_day = timedelta(days=1)
    while day < end:
        if _WEEKDAY_NAMES[day.weekday()] in block.days_of_week:
            start_dt = _local_datetime(day, block.local_start, tz)
            end_day = day + one_day if block.wraps_midnight else day
            end_dt = _local_datetime(end_day, block.local_end, tz)
            intervals.append(
                {
                    "name": block.name,
                    "type": block.type.value,
                    "start_ns": _to_ns(start_dt),
                    "end_ns": _to_ns(end_dt),
                    "_start_date": day,  # dropped before writing; holiday key only
                }
            )
        day += one_day
    return intervals


def generate(template: CalendarTemplate, start: date, end: date) -> list[dict[str, object]]:
    tz = ZoneInfo(template.timezone)
    all_intervals: list[dict[str, object]] = []
    for block in template.blocks:
        all_intervals.extend(expand_block(block, tz, start, end))

    # Holiday suppression is keyed by the block instance's own start date
    # (CalendarTemplate's documented convention), never by the day it may
    # run into.
    surviving = [iv for iv in all_intervals if iv["_start_date"] not in template.holidays]
    for iv in surviving:
        del iv["_start_date"]
    surviving.sort(key=lambda iv: iv["start_ns"])  # type: ignore[arg-type,return-value]

    for prev, curr in itertools.pairwise(surviving):
        if curr["end_ns"] <= curr["start_ns"]:  # type: ignore[operator]
            raise SystemExit(f"template {template.name}: generated zero/negative-length interval: {curr}")
        if curr["start_ns"] < prev["end_ns"]:  # type: ignore[operator]
            raise SystemExit(
                f"template {template.name}: generated overlap -- {prev} overlaps {curr}. "
                "Refusing to write a contradictory calendar; fix the template."
            )
    return surviving


def main(argv: list[str] | None = None) -> int:
    del argv
    template_dir = ROOT / TEMPLATE_DIR
    out_dir = ROOT / GENERATED_DIR
    out_dir.mkdir(parents=True, exist_ok=True)

    template_paths = sorted(template_dir.glob("*.yaml"))
    if not template_paths:
        print(f"no templates found under {TEMPLATE_DIR}", file=sys.stderr)
        return 2

    for template_path in template_paths:
        rel = template_path.relative_to(ROOT).as_posix()
        template = load_template(ROOT, rel)
        intervals = generate(template, WINDOW_START, WINDOW_END)
        payload = {
            "schema_version": SCHEMA_VERSION,
            "template": template.name,
            "generated_from": rel,
            "window_start": WINDOW_START.isoformat(),
            "window_end": WINDOW_END.isoformat(),
            "intervals": intervals,
        }
        out_path = out_dir / f"{template.name}.json"
        out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {out_path.relative_to(ROOT)} ({len(intervals)} intervals)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
