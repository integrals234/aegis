"""AEGIS-002 -- ``tools/update_status.py`` must not author a catalogue entry
that ``tools/audit_requirements.py`` then rejects.

The tool's own docstring promises it "refuses to write a claim the auditor
would reject ... Writing the claim and checking the claim share predicates
with :mod:`audit_requirements`, so the two cannot drift apart." They had
drifted: ``--blocked-until`` wrote ``verification_blocked_until`` without the
``deferral_history`` ledger the auditor requires to accompany it, so every
obligation registered through the tool failed the very next audit and had to
be hand-patched. Found by the M2 closure matrix; these tests are what stop it
recurring.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
import update_status
from audit_requirements import run_audit

pytestmark = pytest.mark.unit


def _tree(tmp_path: Path, catalogue) -> Path:
    return catalogue(
        requirements=[
            {
                "id": "AEGIS-001",
                "module": "Governance",
                "milestone": "M0",
                "priority": "must",
                "title": "t",
                "description": "d",
                "acceptance": "a",
            }
        ],
        statuses={
            "AEGIS-001": {
                "status": "implemented",
                "implementation": ["requirements/requirements.json"],
                "tests": [],
                "reports": [],
            }
        },
    )


def _entry(root: Path) -> dict:
    doc = json.loads((root / "requirements/implementation_status.json").read_text(encoding="utf-8"))
    return doc["requirements"]["AEGIS-001"]


def _audit(root: Path) -> list[str]:
    """The auditor's errors only -- a passing audit is an empty list, which is
    what every assertion below is actually about."""
    return run_audit(
        req_path=root / "requirements/requirements.json",
        status_path=root / "requirements/implementation_status.json",
        root=root,
    ).errors


def _run(root: Path, *args: str) -> int:
    return update_status.main(
        [
            "AEGIS-001",
            "implemented",
            "--root",
            str(root),
            "--status-path",
            str(root / "requirements/implementation_status.json"),
            *args,
        ]
    )


def test_blocked_until_writes_a_deferral_ledger_the_auditor_accepts(tmp_path, catalogue):
    """The regression itself: registering an obligation must leave the
    catalogue auditable, not broken."""
    root = _tree(tmp_path, catalogue)
    assert (
        _run(
            root,
            "--blocked-until",
            "M5",
            "--residual",
            "needs the risk engine",
            "--recorded-at",
            "M2",
            "--deferral-reason",
            "the acceptance names a risk response and no risk layer exists",
            "--deferral-date",
            "2026-08-10",
        )
        == 0
    )

    entry = _entry(root)
    assert entry["verification_blocked_until"] == "M5"
    history = entry["deferral_history"]
    assert len(history) == 1
    assert history[-1] == {
        "blocked_until": "M5",
        "recorded_at": "M2",
        "date": "2026-08-10",
        "reason": "the acceptance names a risk response and no risk layer exists",
    }
    assert _audit(root) == []


def test_blocked_until_without_a_reason_is_refused(tmp_path, catalogue):
    """An obligation with no recorded why is exactly what the ledger exists to
    prevent, so the tool must refuse rather than write a half-entry."""
    root = _tree(tmp_path, catalogue)
    assert _run(root, "--blocked-until", "M5", "--residual", "r", "--recorded-at", "M2") == 2
    assert "verification_blocked_until" not in _entry(root)


def test_re_dating_an_obligation_appends_a_second_ledger_row(tmp_path, catalogue):
    """"Moving a debt is itself a recorded act" -- the move must show up."""
    root = _tree(tmp_path, catalogue)
    _run(
        root, "--blocked-until", "M3", "--residual", "r1",
        "--recorded-at", "M2", "--deferral-reason", "first dating", "--deferral-date", "2026-08-10",
    )
    _run(
        root, "--blocked-until", "M5", "--residual", "r2",
        "--recorded-at", "M2", "--deferral-reason", "re-dated later", "--deferral-date", "2026-08-11",
    )

    history = _entry(root)["deferral_history"]
    assert [row["blocked_until"] for row in history] == ["M3", "M5"]
    assert _entry(root)["verification_blocked_until"] == "M5"
    assert _audit(root) == []


def test_re_recording_the_same_milestone_does_not_inflate_the_ledger(tmp_path, catalogue):
    """The ledger counts moves. Rewriting an unchanged obligation is not one,
    and a duplicated row would report a re-dating that never happened."""
    root = _tree(tmp_path, catalogue)
    for _ in range(3):
        _run(
            root, "--blocked-until", "M3", "--residual", "r",
            "--recorded-at", "M2", "--deferral-reason", "same dating",
            "--deferral-date", "2026-08-10",
        )
    assert len(_entry(root)["deferral_history"]) == 1
    assert _audit(root) == []


def test_clear_obligation_drops_the_ledger_with_the_live_field(tmp_path, catalogue):
    """A ledger with no live obligation to explain is a dangling record the
    auditor rejects, so discharging must remove both together."""
    root = _tree(tmp_path, catalogue)
    _run(
        root, "--blocked-until", "M3", "--residual", "r",
        "--recorded-at", "M2", "--deferral-reason", "why", "--deferral-date", "2026-08-10",
    )
    assert _run(root, "--clear-obligation") == 0

    entry = _entry(root)
    assert "verification_blocked_until" not in entry
    assert "residual" not in entry
    assert "deferral_history" not in entry
    assert _audit(root) == []
