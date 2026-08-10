#!/usr/bin/env python3
"""Generate AEGIS-013 evidence from the committed, generated calendars.

Calls the real production path -- ``futures.calendars.load_calendar_registry``
and ``.classify`` -- across a fixed set of boundary timestamps for all three
committed templates, including both 2026 US DST transitions, so the evidence
proves what the tests prove rather than a parallel claim about it.

Regenerate with: python3 tools/generate_session_evidence.py
Evidence must be regenerated from a clean, committed worktree -- a dirty
capture is recorded honestly (``dirty: true``) rather than concealed.
"""

from __future__ import annotations

import json
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from futures.calendars import CalendarRegistry, load_calendar_registry


def _git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else ""


def _provenance() -> dict[str, Any]:
    return {
        "generated_on": datetime.now(UTC).strftime("%Y-%m-%d"),
        "repository_commit": _git("rev-parse", "HEAD"),
        "dirty": bool(_git("status", "--porcelain")),
    }


def _ns(dt: datetime) -> int:
    delta = dt.astimezone(UTC) - datetime(1970, 1, 1, tzinfo=UTC)
    return (delta.days * 86400 + delta.seconds) * 1_000_000_000


def _case(label: str, as_of: datetime, template: str, registry: CalendarRegistry) -> dict[str, Any]:
    as_of_ns = _ns(as_of)
    result = registry.classify(as_of_ns, template)
    return {
        "case": label,
        "template": template,
        "as_of_utc": as_of.astimezone(UTC).isoformat(),
        "as_of_ns": as_of_ns,
        "state": result.state.value,
        "session_name": result.session_name,
    }


def build_cases(registry: CalendarRegistry) -> list[dict[str, Any]]:
    chicago = ZoneInfo("America/Chicago")
    new_york = ZoneInfo("America/New_York")
    cases: list[dict[str, Any]] = []

    # Normal.
    cases.append(_case("normal_regular_session", datetime(2026, 3, 10, 9, 0, tzinfo=chicago),
                        "synx_equity_index_rth", registry))
    cases.append(_case("normal_closed_overnight_gap", datetime(2026, 3, 10, 3, 0, tzinfo=UTC),
                        "synx_equity_index_rth", registry))

    # Holiday.
    cases.append(_case("holiday_closed", datetime(2026, 1, 1, 9, 0, tzinfo=chicago),
                        "synx_equity_index_rth", registry))
    cases.append(_case("day_after_holiday_normal", datetime(2026, 1, 2, 9, 0, tzinfo=chicago),
                        "synx_equity_index_rth", registry))

    # Overnight / maintenance.
    cases.append(_case("overnight_session", datetime(2026, 3, 10, 20, 0, tzinfo=new_york),
                        "synx_energy_extended", registry))
    cases.append(_case("maintenance_window", datetime(2026, 3, 10, 17, 30, tzinfo=new_york),
                        "synx_energy_extended", registry))
    cases.append(_case("weekend_closed", datetime(2026, 3, 14, 12, 0, tzinfo=new_york),
                        "synx_energy_extended", registry))

    # DST -- both templates that observe DST, both transitions.
    for template, tz in (("synx_equity_index_rth", chicago), ("synx_rates_globex", chicago),
                          ("synx_energy_extended", new_york)):
        for label, day in (
            ("dst_spring_forward_before", datetime(2026, 3, 6, 9, 0, tzinfo=tz)),
            ("dst_spring_forward_after", datetime(2026, 3, 9, 9, 0, tzinfo=tz)),
            ("dst_fall_back_before", datetime(2026, 10, 30, 9, 0, tzinfo=tz)),
            ("dst_fall_back_after", datetime(2026, 11, 2, 9, 0, tzinfo=tz)),
        ):
            cases.append(_case(label, day, template, registry))

    # Exactly-open / exactly-close.
    interval = registry.intervals("synx_equity_index_rth")[0]
    cases.append(
        {
            "case": "exactly_open",
            "template": "synx_equity_index_rth",
            "as_of_ns": interval.start_ns,
            "as_of_utc": datetime.fromtimestamp(interval.start_ns / 1e9, tz=UTC).isoformat(),
            "state": registry.classify(interval.start_ns, "synx_equity_index_rth").state.value,
            "session_name": registry.classify(interval.start_ns, "synx_equity_index_rth").session_name,
        }
    )
    cases.append(
        {
            "case": "exactly_close",
            "template": "synx_equity_index_rth",
            "as_of_ns": interval.end_ns,
            "as_of_utc": datetime.fromtimestamp(interval.end_ns / 1e9, tz=UTC).isoformat(),
            "state": registry.classify(interval.end_ns, "synx_equity_index_rth").state.value,
            "session_name": registry.classify(interval.end_ns, "synx_equity_index_rth").session_name,
        }
    )

    return cases


def main(argv: list[str] | None = None) -> int:
    del argv
    registry = load_calendar_registry(ROOT)
    cases = build_cases(registry)

    payload = {
        "artifact": "session_classification",
        "requirement": "AEGIS-013",
        **_provenance(),
        "templates": registry.templates(),
        "case_count": len(cases),
        "cases": cases,
        "claim": (
            f"{len(cases)} boundary timestamps were classified through the real "
            "CalendarRegistry.classify() production path (no reimplementation), covering "
            "normal, holiday, overnight, maintenance, weekend-closed, exactly-open, "
            "exactly-close and both 2026 US DST transitions (spring-forward 2026-03-08, "
            "fall-back 2026-11-01) across all three committed synthetic templates. All data "
            "is synthetic (DATA_AND_RESEARCH_POLICY); no claim is made about any real "
            "exchange calendar."
        ),
    }

    out_dir = ROOT / "experiments/evidence/AEGIS-013"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "session_classification.json"
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes, {len(cases)} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
