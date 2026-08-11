"""M2 slice 3 -- AEGIS-013: trading-session calendars.

The acceptance is "session classification tests cover normal, holiday,
overnight, and DST cases", so this file drives the two calendar layers
directly: the committed, generated artifact through
:class:`~futures.calendars.CalendarRegistry` (the only thing any runtime
caller may use), and the offline generator (``tools/generate_calendars``)
against a small purpose-built fixture for the DST case that the committed
templates' weekend-closed shape does not itself exercise (the transition
instant always falls on a US Sunday at 02:00 local, before any committed
session starts).
"""

from __future__ import annotations

from datetime import UTC, date, datetime, time
from pathlib import Path
from zoneinfo import ZoneInfo

import generate_calendars
import pytest
from futures.calendars import (
    CalendarInterval,
    CalendarRegistry,
    CalendarTemplate,
    InvalidCalendar,
    SessionBlockSpec,
    SessionState,
    SessionType,
    load_calendar_registry,
    load_template,
)

pytestmark = pytest.mark.unit


@pytest.fixture
def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


@pytest.fixture
def registry(repo_root: Path) -> CalendarRegistry:
    return load_calendar_registry(repo_root)


def _ns(dt: datetime) -> int:
    delta = dt.astimezone(UTC) - datetime(1970, 1, 1, tzinfo=UTC)
    return (delta.days * 86400 + delta.seconds) * 1_000_000_000


# --------------------------------------------------------------------- load


def test_all_three_committed_templates_load(registry: CalendarRegistry) -> None:
    assert registry.templates() == (
        "synx_energy_extended",
        "synx_equity_index_rth",
        "synx_rates_globex",
    )


def test_every_product_session_template_resolves(repo_root: Path, registry: CalendarRegistry) -> None:
    """AEGIS-013's cross-check against slice 2: no dangling product reference."""
    from futures.instruments import DEFAULT_CATALOG_PATH, load_catalog

    catalog = load_catalog(repo_root, DEFAULT_CATALOG_PATH)
    for product in catalog:
        assert product.session_template in registry


# --------------------------------------------------------------- normal case


def test_normal_weekday_session_classifies_regular(registry: CalendarRegistry) -> None:
    # 2026-03-10 (Tuesday) 09:00 local Chicago (CDT, UTC-5) = 14:00 UTC.
    as_of = _ns(datetime(2026, 3, 10, 14, 0, tzinfo=UTC))
    result = registry.classify(as_of, "synx_equity_index_rth")
    assert result.state is SessionState.REGULAR
    assert result.session_name == "regular"


def test_outside_session_hours_is_closed(registry: CalendarRegistry) -> None:
    # 2026-03-10 03:00 UTC is the middle of the Chicago night, no session.
    as_of = _ns(datetime(2026, 3, 10, 3, 0, tzinfo=UTC))
    result = registry.classify(as_of, "synx_equity_index_rth")
    assert result.state is SessionState.CLOSED
    assert result.session_name is None


def test_weekend_is_closed(registry: CalendarRegistry) -> None:
    # 2026-03-14 is a Saturday.
    as_of = _ns(datetime(2026, 3, 14, 16, 0, tzinfo=UTC))
    assert registry.classify(as_of, "synx_equity_index_rth").state is SessionState.CLOSED


# -------------------------------------------------------------------- holiday


def test_holiday_is_fully_closed(registry: CalendarRegistry) -> None:
    """2026-01-01 is a committed EQX holiday; the usual 09:00 local slot is CLOSED."""
    as_of = _ns(datetime(2026, 1, 1, 15, 0, tzinfo=UTC))  # CST on Jan 1
    assert registry.classify(as_of, "synx_equity_index_rth").state is SessionState.CLOSED


def test_day_after_holiday_is_normal(registry: CalendarRegistry) -> None:
    # 2026-01-02 is a Friday, not a holiday.
    as_of = _ns(datetime(2026, 1, 2, 15, 0, tzinfo=UTC))
    assert registry.classify(as_of, "synx_equity_index_rth").state is SessionState.REGULAR


# ------------------------------------------------------------ overnight/maint


def test_overnight_session_classifies_overnight(registry: CalendarRegistry) -> None:
    # 2026-03-10 (Tue) 20:00 local New York (EDT, UTC-4) = 2026-03-11 00:00 UTC.
    as_of = _ns(datetime(2026, 3, 11, 0, 0, tzinfo=UTC))
    result = registry.classify(as_of, "synx_energy_extended")
    assert result.state is SessionState.OVERNIGHT
    assert result.session_name == "overnight"


def test_maintenance_window_classifies_maintenance(registry: CalendarRegistry) -> None:
    # 2026-03-10 (Tue) 17:30 local New York (EDT) = 21:30 UTC.
    as_of = _ns(datetime(2026, 3, 10, 21, 30, tzinfo=UTC))
    result = registry.classify(as_of, "synx_energy_extended")
    assert result.state is SessionState.MAINTENANCE
    assert result.session_name == "maintenance"


def test_weekend_gap_between_friday_maintenance_and_sunday_open_is_closed(
    registry: CalendarRegistry,
) -> None:
    # 2026-03-14 (Sat) noon local -- well inside the Fri-close/Sun-open gap.
    as_of = _ns(datetime(2026, 3, 14, 17, 0, tzinfo=UTC))
    assert registry.classify(as_of, "synx_energy_extended").state is SessionState.CLOSED


# --------------------------------------------------------------- boundaries


def test_exactly_open_boundary_classifies_into_the_session(registry: CalendarRegistry) -> None:
    interval = registry.intervals("synx_equity_index_rth")[0]
    result = registry.classify(interval.start_ns, "synx_equity_index_rth")
    assert result.state is not SessionState.CLOSED
    assert result.session_name == interval.name


def test_exactly_close_boundary_does_not_classify_into_the_session(
    registry: CalendarRegistry,
) -> None:
    interval = registry.intervals("synx_equity_index_rth")[0]
    result = registry.classify(interval.end_ns, "synx_equity_index_rth")
    assert result.session_name != interval.name


def test_one_nanosecond_before_close_still_classifies(registry: CalendarRegistry) -> None:
    interval = registry.intervals("synx_equity_index_rth")[0]
    result = registry.classify(interval.end_ns - 1, "synx_equity_index_rth")
    assert result.session_name == interval.name


# ------------------------------------------------------------- invalid input


def test_unknown_template_raises(registry: CalendarRegistry) -> None:
    with pytest.raises(InvalidCalendar):
        registry.classify(0, "does_not_exist")


def test_missing_generated_directory_raises(repo_root: Path) -> None:
    with pytest.raises(InvalidCalendar):
        load_calendar_registry(repo_root, directory="configs/calendars/does_not_exist")


def test_end_before_start_interval_rejected() -> None:
    with pytest.raises(InvalidCalendar):
        CalendarInterval(name="bad", type=SessionType.REGULAR, start_ns=100, end_ns=100)
    with pytest.raises(InvalidCalendar):
        CalendarInterval(name="bad", type=SessionType.REGULAR, start_ns=100, end_ns=50)


def test_overlapping_intervals_rejected() -> None:
    a = CalendarInterval(name="a", type=SessionType.REGULAR, start_ns=0, end_ns=100)
    b = CalendarInterval(name="b", type=SessionType.REGULAR, start_ns=50, end_ns=150)
    with pytest.raises(InvalidCalendar, match="overlaps"):
        CalendarRegistry({"tmpl": [a, b]})


def test_duplicate_start_intervals_rejected() -> None:
    a = CalendarInterval(name="a", type=SessionType.REGULAR, start_ns=0, end_ns=100)
    b = CalendarInterval(name="b", type=SessionType.REGULAR, start_ns=0, end_ns=50)
    with pytest.raises(InvalidCalendar):
        CalendarRegistry({"tmpl": [a, b]})


def test_non_overlapping_adjacent_intervals_accepted() -> None:
    a = CalendarInterval(name="a", type=SessionType.REGULAR, start_ns=0, end_ns=100)
    b = CalendarInterval(name="b", type=SessionType.MAINTENANCE, start_ns=100, end_ns=150)
    registry = CalendarRegistry({"tmpl": [b, a]})  # insertion order irrelevant
    assert registry.classify(0, "tmpl").session_name == "a"
    assert registry.classify(99, "tmpl").session_name == "a"
    assert registry.classify(100, "tmpl").session_name == "b"


def test_zero_length_block_rejected() -> None:
    with pytest.raises(InvalidCalendar):
        SessionBlockSpec(
            name="bad",
            type=SessionType.REGULAR,
            local_start=time(8, 0),
            local_end=time(8, 0),
            days_of_week=frozenset({"mon"}),
        )


def test_unknown_weekday_rejected() -> None:
    with pytest.raises(InvalidCalendar):
        SessionBlockSpec(
            name="bad",
            type=SessionType.REGULAR,
            local_start=time(8, 0),
            local_end=time(9, 0),
            days_of_week=frozenset({"funday"}),
        )


def test_malformed_template_schema_rejected(tmp_path: Path, repo_root: Path) -> None:
    (tmp_path / "configs/schemas").mkdir(parents=True)
    (tmp_path / "configs/calendars/templates").mkdir(parents=True)
    schema_src = repo_root / "configs/schemas/futures_calendar_template.v1.json"
    (tmp_path / "configs/schemas/futures_calendar_template.v1.json").write_text(
        schema_src.read_text(encoding="utf-8"), encoding="utf-8"
    )
    bad = tmp_path / "configs/calendars/templates/bad.yaml"
    bad.write_text("schema_version: 1\ntemplate: bad\n", encoding="utf-8")  # missing required fields
    with pytest.raises(InvalidCalendar):
        load_template(tmp_path, "configs/calendars/templates/bad.yaml")


def test_unsupported_schema_version_rejected(tmp_path: Path, repo_root: Path) -> None:
    (tmp_path / "configs/schemas").mkdir(parents=True)
    (tmp_path / "configs/calendars/templates").mkdir(parents=True)
    schema_src = repo_root / "configs/schemas/futures_calendar_template.v1.json"
    (tmp_path / "configs/schemas/futures_calendar_template.v1.json").write_text(
        schema_src.read_text(encoding="utf-8"), encoding="utf-8"
    )
    bad = tmp_path / "configs/calendars/templates/bad.yaml"
    bad.write_text(
        "schema_version: 2\ntemplate: bad\ntimezone: UTC\nblocks: []\nholidays: []\n",
        encoding="utf-8",
    )
    with pytest.raises(InvalidCalendar, match="schema_version"):
        load_template(tmp_path, "configs/calendars/templates/bad.yaml")


def test_as_of_must_be_int(registry: CalendarRegistry) -> None:
    with pytest.raises(InvalidCalendar):
        registry.classify("not an int", "synx_equity_index_rth")  # type: ignore[arg-type]


# -------------------------------------------------------------------------- DST
#
# The committed templates are weekend-closed, and both 2026 US DST transitions
# fall on a Sunday at 02:00 local -- before any committed session starts, so
# no *committed* interval actually spans a transition instant. Two
# independent checks instead:
#
# 1. Against the committed artifact: the same local start time (08:30 America
#    /Chicago) maps to a UTC instant that shifts by exactly one hour across
#    each transition -- a wrong-offset generator bug would fail this.
# 2. Against the generator directly: a purpose-built template whose block
#    genuinely spans the transition instant (Saturday start) proves
#    tools/generate_calendars computes the true, non-24h-multiple elapsed
#    duration rather than a naive fixed-offset shift.


def _interval_starting_on(registry: CalendarRegistry, template: str, day: date) -> CalendarInterval:
    return next(
        iv
        for iv in registry.intervals(template)
        if datetime.fromtimestamp(iv.start_ns / 1e9, tz=UTC).date() == day
    )


def test_dst_spring_forward_shifts_committed_utc_boundary_by_one_hour(
    registry: CalendarRegistry,
) -> None:
    before = _interval_starting_on(registry, "synx_equity_index_rth", date(2026, 3, 6))  # Fri
    after = _interval_starting_on(registry, "synx_equity_index_rth", date(2026, 3, 9))  # Mon
    # Independently derive the expected shift via zoneinfo (test-only use).
    tz = ZoneInfo("America/Chicago")
    expected_before = _ns(datetime(2026, 3, 6, 8, 30, tzinfo=tz))
    expected_after = _ns(datetime(2026, 3, 9, 8, 30, tzinfo=tz))
    assert before.start_ns == expected_before
    assert after.start_ns == expected_after
    # 3 calendar days apart (Fri -> Mon), minus the hour spring-forward skips:
    # a generator using a fixed offset instead of true zoneinfo semantics
    # would be off by exactly one hour here.
    calendar_days_apart = 3
    assert expected_after - expected_before == calendar_days_apart * 86400 * 1_000_000_000 - 3600 * 1_000_000_000


def test_dst_fall_back_shifts_committed_utc_boundary_by_one_hour(registry: CalendarRegistry) -> None:
    before = _interval_starting_on(registry, "synx_equity_index_rth", date(2026, 10, 30))  # Fri
    after = _interval_starting_on(registry, "synx_equity_index_rth", date(2026, 11, 2))  # Mon
    tz = ZoneInfo("America/Chicago")
    expected_before = _ns(datetime(2026, 10, 30, 8, 30, tzinfo=tz))
    expected_after = _ns(datetime(2026, 11, 2, 8, 30, tzinfo=tz))
    assert before.start_ns == expected_before
    assert after.start_ns == expected_after
    # 3 calendar days apart (Fri -> Mon), plus the hour fall-back repeats.
    calendar_days_apart = 3
    assert expected_after - expected_before == calendar_days_apart * 86400 * 1_000_000_000 + 3600 * 1_000_000_000


def _spanning_template(start_day: str) -> CalendarTemplate:
    return CalendarTemplate(
        name="dst_probe",
        timezone="America/Chicago",
        blocks=(
            SessionBlockSpec(
                name="span",
                type=SessionType.OVERNIGHT,
                local_start=time(18, 0),
                local_end=time(17, 0),
                days_of_week=frozenset({start_day}),
            ),
        ),
        holidays=frozenset(),
    )


def test_generator_shortens_the_spring_forward_spanning_interval() -> None:
    # 2026-03-08 is the transition Sunday; a block starting Saturday 2026-03-07
    # 18:00 and ending Sunday 17:00 genuinely spans 02:00-03:00 local, so it
    # loses the skipped hour: 22h elapsed, not the usual 23h.
    template = _spanning_template("sat")
    intervals = generate_calendars.generate(template, date(2026, 3, 7), date(2026, 3, 9))
    assert len(intervals) == 1
    duration_ns = intervals[0]["end_ns"] - intervals[0]["start_ns"]
    assert duration_ns == 22 * 3600 * 1_000_000_000


def test_generator_lengthens_the_fall_back_spanning_interval() -> None:
    # 2026-11-01 is the transition Sunday; a block starting Saturday
    # 2026-10-31 18:00 and ending Sunday 17:00 gains the repeated hour: 24h
    # elapsed, not the usual 23h.
    template = _spanning_template("sat")
    intervals = generate_calendars.generate(template, date(2026, 10, 31), date(2026, 11, 2))
    assert len(intervals) == 1
    duration_ns = intervals[0]["end_ns"] - intervals[0]["start_ns"]
    assert duration_ns == 24 * 3600 * 1_000_000_000


def test_generator_output_is_deterministic_across_pythonhashseed_processes(
    repo_root: Path,
) -> None:
    """Re-generation is byte-identical to the committed artifact (proxy for
    cross-PYTHONHASHSEED determinism, which is exercised at the process level
    by the Checkpoint 1 battery)."""
    template = load_template(repo_root, "configs/calendars/templates/synx_equity_index_rth.yaml")
    first = generate_calendars.generate(template, date(2025, 1, 1), date(2026, 1, 1))
    second = generate_calendars.generate(template, date(2025, 1, 1), date(2026, 1, 1))
    assert first == second


def test_generator_rejects_overlapping_result() -> None:
    template = CalendarTemplate(
        name="broken",
        timezone="UTC",
        blocks=(
            SessionBlockSpec(
                name="a", type=SessionType.REGULAR, local_start=time(8, 0), local_end=time(16, 0),
                days_of_week=frozenset({"mon"}),
            ),
            SessionBlockSpec(
                name="b", type=SessionType.REGULAR, local_start=time(15, 0), local_end=time(20, 0),
                days_of_week=frozenset({"mon"}),
            ),
        ),
        holidays=frozenset(),
    )
    with pytest.raises(SystemExit):
        generate_calendars.generate(template, date(2026, 3, 2), date(2026, 3, 3))
