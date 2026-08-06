"""AEGIS-003 — a verification obligation may not be moved without recording the move.

The M0 audit found three obligations dated ``M1`` whose own acceptance criteria
depended on work the architecture rules place at M3, M4 and M5. Re-dating them was
correct. What was missing is that re-dating them cost nothing: ``verification_blocked_until``
is one scalar, and editing it left no trace that a MUST requirement's debt had been
pushed to a later milestone. Repeat that quietly enough times and a MUST requirement
is deferred forever while every gate stays green — the exact outcome AEGIS-003 exists
to prevent.

So the obligation now carries an append-only ledger, and the audit requires the live
field to agree with the head of it. Moving a debt is therefore an edit that must be
written down, and the register prints how many times each one has moved.

The second half of this file covers the companion rule: an artifact sitting in
``experiments/evidence/<RID>/`` that no status entry cites is re-checked by nothing.
That is how the stale ``AEGIS-233/test_layers.json`` (318 unit tests recorded against
a tree with 321) survived the milestone unnoticed.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from audit_requirements import run_audit

pytestmark = pytest.mark.unit


def requirement(rid: str = "AEGIS-001") -> dict:
    return {
        "id": rid,
        "module": "Governance",
        "milestone": "M0",
        "priority": "must",
        "title": "title",
        "description": "description",
        "acceptance": "acceptance",
    }


def entry(status: str = "implemented", **extra: object) -> dict:
    base = {
        "status": status,
        "implementation": ["tools/audit_requirements.py"],
        "tests": [],
        "reports": [],
    }
    base.update(extra)
    return base


def ledger(*pairs: tuple[str, str]) -> list[dict]:
    return [
        {
            "blocked_until": blocked,
            "recorded_at": "M0",
            "date": date,
            "reason": "the acceptance names something that does not exist yet",
        }
        for blocked, date in pairs
    ]


def audit(root: Path, **kwargs: object):
    return run_audit(
        req_path=root / "requirements/requirements.json",
        status_path=root / "requirements/implementation_status.json",
        root=root,
        **kwargs,  # type: ignore[arg-type]
    )


def blocked_entry(blocked: str = "M1", **extra: object) -> dict:
    return entry(
        verification_blocked_until=blocked,
        residual="no engine exists to constrain",
        deferral_history=ledger((blocked, "2026-08-06")),
        **extra,
    )


# --------------------------------------------------------------- the ledger


def test_a_well_formed_obligation_passes(catalogue, tmp_path):
    root = catalogue([requirement()], {"AEGIS-001": blocked_entry()})
    (root / "tools").mkdir(parents=True, exist_ok=True)
    (root / "tools/audit_requirements.py").write_text("x = 1\n", encoding="utf-8")
    assert audit(root).ok


def test_an_obligation_without_a_ledger_is_rejected(catalogue):
    """The whole point: `verification_blocked_until` alone is no longer enough."""
    statuses = {
        "AEGIS-001": entry(
            verification_blocked_until="M1",
            residual="no engine exists to constrain",
        )
    }
    root = catalogue([requirement()], statuses)
    result = audit(root)
    assert not result.ok
    assert any("requires a deferral_history" in e for e in result.errors)


def test_moving_an_obligation_without_recording_the_move_is_rejected(catalogue):
    """Editing the scalar from M1 to M4 while the ledger still says M1."""
    statuses = {
        "AEGIS-001": entry(
            verification_blocked_until="M4",
            residual="no strategy code exists to constrain",
            deferral_history=ledger(("M1", "2026-08-06")),
        )
    }
    root = catalogue([requirement()], statuses)
    result = audit(root)
    assert not result.ok
    assert any("cannot be moved without recording the move" in e for e in result.errors)


def test_a_recorded_move_is_accepted(catalogue):
    statuses = {
        "AEGIS-001": entry(
            verification_blocked_until="M4",
            residual="no strategy code exists to constrain",
            deferral_history=ledger(("M1", "2026-08-06"), ("M4", "2026-08-06")),
        )
    }
    root = catalogue([requirement()], statuses)
    (root / "tools").mkdir(parents=True, exist_ok=True)
    (root / "tools/audit_requirements.py").write_text("x = 1\n", encoding="utf-8")
    assert audit(root).ok


def test_a_ledger_without_an_obligation_is_rejected(catalogue):
    """History left behind after the debt was paid would misreport the register."""
    statuses = {"AEGIS-001": entry(deferral_history=ledger(("M1", "2026-08-06")))}
    root = catalogue([requirement()], statuses)
    result = audit(root)
    assert not result.ok
    assert any("deferral_history recorded without" in e for e in result.errors)


@pytest.mark.parametrize(
    ("mutation", "expected"),
    [
        ({"blocked_until": "M99"}, "must be a milestone ID"),
        ({"recorded_at": "later"}, "must be a milestone ID"),
        ({"date": "6 August"}, "must be an ISO date"),
        ({"reason": "   "}, "must say why"),
    ],
)
def test_malformed_ledger_entries_are_rejected(catalogue, mutation, expected):
    item = ledger(("M1", "2026-08-06"))[0]
    item.update(mutation)
    statuses = {
        "AEGIS-001": entry(
            verification_blocked_until=item["blocked_until"],
            residual="no engine exists to constrain",
            deferral_history=[item],
        )
    }
    root = catalogue([requirement()], statuses)
    result = audit(root)
    assert not result.ok
    assert any(expected in e for e in result.errors), result.errors


def test_ledger_entries_must_not_go_backwards_in_time(catalogue):
    statuses = {
        "AEGIS-001": entry(
            verification_blocked_until="M4",
            residual="no strategy code exists to constrain",
            deferral_history=ledger(("M1", "2026-08-06"), ("M4", "2026-01-01")),
        )
    }
    root = catalogue([requirement()], statuses)
    result = audit(root)
    assert not result.ok
    assert any("earlier than the entry before it" in e for e in result.errors)


def test_unknown_ledger_fields_are_rejected(catalogue):
    item = ledger(("M1", "2026-08-06"))[0]
    item["approved_by"] = "nobody"
    statuses = {
        "AEGIS-001": entry(
            verification_blocked_until="M1",
            residual="no engine exists to constrain",
            deferral_history=[item],
        )
    }
    root = catalogue([requirement()], statuses)
    result = audit(root)
    assert not result.ok
    assert any("unknown fields" in e for e in result.errors)


def test_check_deferred_reports_how_many_times_an_obligation_moved(catalogue):
    statuses = {
        "AEGIS-001": entry(
            verification_blocked_until="M4",
            residual="no strategy code exists to constrain",
            deferral_history=ledger(("M1", "2026-08-06"), ("M4", "2026-08-06")),
        )
    }
    root = catalogue([requirement()], statuses)
    result = audit(root, check_deferred="M4")
    assert not result.ok
    assert any("re-dated 1 time(s)" in e for e in result.errors)


# ------------------------------------------------- unregistered evidence


def evidence_root(root: Path, rid: str, name: str, content: str = '{"ok": true}\n') -> Path:
    directory = root / "experiments/evidence" / rid
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    path.write_text(content, encoding="utf-8")
    return path


def test_an_uncited_evidence_artifact_is_rejected(catalogue):
    """The stale-artifact failure mode, as a gate."""
    root = catalogue([requirement()], {"AEGIS-001": entry()})
    (root / "tools").mkdir(parents=True, exist_ok=True)
    (root / "tools/audit_requirements.py").write_text("x = 1\n", encoding="utf-8")
    evidence_root(root, "AEGIS-001", "test_layers.json")

    result = audit(root)
    assert not result.ok
    assert any("is not registered in implementation_status.json" in e for e in result.errors)


def test_a_cited_evidence_artifact_passes(catalogue):
    statuses = {
        "AEGIS-001": entry(reports=["experiments/evidence/AEGIS-001/test_layers.json"])
    }
    root = catalogue([requirement()], statuses)
    (root / "tools").mkdir(parents=True, exist_ok=True)
    (root / "tools/audit_requirements.py").write_text("x = 1\n", encoding="utf-8")
    evidence_root(root, "AEGIS-001", "test_layers.json")
    assert audit(root).ok


def test_quick_mode_skips_the_artifact_sweep(catalogue):
    """`--quick` is the pre-commit hook; it stays structural and fast."""
    root = catalogue([requirement()], {"AEGIS-001": entry()})
    (root / "tools").mkdir(parents=True, exist_ok=True)
    (root / "tools/audit_requirements.py").write_text("x = 1\n", encoding="utf-8")
    evidence_root(root, "AEGIS-001", "test_layers.json")
    assert audit(root, deep=False).ok


def test_the_live_register_matches_the_live_tracker(repo_root):
    """docs/DEFERRED_VERIFICATION.md is generated; drift would misinform a reader."""
    import generate_deferred_register

    reqs = json.loads((repo_root / "requirements/requirements.json").read_text(encoding="utf-8"))
    statuses = json.loads(
        (repo_root / "requirements/implementation_status.json").read_text(encoding="utf-8")
    )
    rendered = generate_deferred_register.render(reqs["requirements"], statuses["requirements"])
    on_disk = (repo_root / "docs/DEFERRED_VERIFICATION.md").read_text(encoding="utf-8")
    assert rendered == on_disk
