"""AEGIS-003 — a completion claim must be backed by an artifact that says something.

The failure this file exists to prevent: a requirement marked ``verified`` whose
evidence list points at a directory, an empty file, a ``.gitkeep`` or a file
containing nothing but TODOs. Each is a path that exists, and existence alone
was the previous bar.
"""

from __future__ import annotations

from pathlib import Path

import pytest
import update_status
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


AUDIT_RECORD = {"auditor": "aegis-spec-auditor", "commit": "abc1234", "date": "2026-08-05"}


def entry(status: str, **extra: object) -> dict:
    base = {"status": status, "implementation": [], "tests": [], "reports": []}
    base.update(extra)
    return base


def audit(root: Path, **kwargs: object):
    return run_audit(
        req_path=root / "requirements/requirements.json",
        status_path=root / "requirements/implementation_status.json",
        root=root,
        **kwargs,  # type: ignore[arg-type]
    )


def write(root: Path, rel: str, content: str = "assert True\n") -> str:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return rel


def test_verified_with_real_evidence_passes(catalogue):
    statuses = {
        "AEGIS-001": entry(
            "verified",
            implementation=["tools/thing.py"],
            tests=["tests/unit/test_thing.py"],
            audit=AUDIT_RECORD,
        )
    }
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "def thing():\n    return 1\n")
    write(root, "tests/unit/test_thing.py", "def test_thing():\n    assert thing() == 1\n")
    assert audit(root).ok


def test_nonexistent_evidence_path_is_rejected(catalogue):
    statuses = {
        "AEGIS-001": entry(
            "verified",
            implementation=["tools/missing.py"],
            tests=["tests/missing.py"],
            audit=AUDIT_RECORD,
        )
    }
    root = catalogue([requirement()], statuses)
    result = audit(root)
    assert any("evidence path does not exist: tools/missing.py" in e for e in result.errors)


def test_empty_file_is_not_evidence(catalogue):
    statuses = {
        "AEGIS-001": entry(
            "verified",
            implementation=["tools/thing.py"],
            tests=["tests/unit/test_empty.py"],
            audit=AUDIT_RECORD,
        )
    }
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    write(root, "tests/unit/test_empty.py", "")
    result = audit(root)
    assert any("is empty" in e for e in result.errors)


def test_directory_is_not_evidence(catalogue):
    statuses = {
        "AEGIS-001": entry(
            "verified", implementation=["tools/thing.py"], tests=["tests/unit"], audit=AUDIT_RECORD
        )
    }
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    (root / "tests/unit").mkdir(parents=True, exist_ok=True)
    result = audit(root)
    assert any("is a directory" in e for e in result.errors)


def test_gitkeep_is_not_evidence(catalogue):
    statuses = {
        "AEGIS-001": entry(
            "verified",
            implementation=["tools/thing.py"],
            tests=["tests/unit/.gitkeep"],
            audit=AUDIT_RECORD,
        )
    }
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    write(root, "tests/unit/.gitkeep", "\n")
    result = audit(root)
    assert any("placeholder file" in e for e in result.errors)


def test_todo_only_file_is_not_evidence(catalogue):
    statuses = {
        "AEGIS-001": entry(
            "verified",
            implementation=["tools/thing.py"],
            tests=["tests/unit/test_todo.py"],
            audit=AUDIT_RECORD,
        )
    }
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    write(root, "tests/unit/test_todo.py", "TODO: write this test\nFIXME: and this one\n")
    result = audit(root)
    assert any("only TODO/placeholder text" in e for e in result.errors)


def test_verified_requires_an_audit_record(catalogue):
    statuses = {"AEGIS-001": entry("verified", implementation=["tools/thing.py"], tests=["tests/unit/test_thing.py"])}
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    write(root, "tests/unit/test_thing.py", "def test_thing():\n    assert True\n")
    result = audit(root)
    assert any("requires an 'audit' object" in e for e in result.errors)


def test_verified_requires_test_or_report_evidence(catalogue):
    statuses = {"AEGIS-001": entry("verified", implementation=["tools/thing.py"], audit=AUDIT_RECORD)}
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    result = audit(root)
    assert any("verified without test/report evidence" in e for e in result.errors)


def test_implemented_requires_a_substantive_implementation_path(catalogue):
    statuses = {"AEGIS-001": entry("implemented", implementation=["cpp/common/.gitkeep"])}
    root = catalogue([requirement()], statuses)
    write(root, "cpp/common/.gitkeep", "")
    result = audit(root)
    assert any("no implementation path is a non-empty, non-placeholder file" in e for e in result.errors)


def test_quick_mode_skips_content_inspection(catalogue):
    """--quick is a fast structural pass; it must not silently become the gate."""
    statuses = {
        "AEGIS-001": entry(
            "verified",
            implementation=["tools/thing.py"],
            tests=["tests/unit/test_empty.py"],
            audit=AUDIT_RECORD,
        )
    }
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    write(root, "tests/unit/test_empty.py", "")
    assert audit(root, deep=False).ok
    assert not audit(root, deep=True).ok


def test_verified_is_blocked_while_an_obligation_is_registered(catalogue):
    statuses = {
        "AEGIS-001": entry(
            "verified",
            implementation=["tools/thing.py"],
            tests=["tests/unit/test_thing.py"],
            audit=AUDIT_RECORD,
            verification_blocked_until="M1",
            residual="no exchange code exists yet",
        )
    }
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    write(root, "tests/unit/test_thing.py", "def test_thing():\n    assert True\n")
    result = audit(root)
    assert any("cannot be 'verified' while verification is blocked until M1" in e for e in result.errors)


def test_obligation_requires_a_residual(catalogue):
    statuses = {
        "AEGIS-001": entry("implemented", implementation=["tools/thing.py"], verification_blocked_until="M1")
    }
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    result = audit(root)
    assert any("requires a residual" in e for e in result.errors)


def test_check_deferred_fails_when_an_obligation_comes_due(catalogue):
    statuses = {
        "AEGIS-001": entry(
            "implemented",
            implementation=["tools/thing.py"],
            verification_blocked_until="M1",
            residual="no exchange code exists yet",
            deferral_history=[
                {
                    "blocked_until": "M1",
                    "recorded_at": "M0",
                    "date": "2026-08-06",
                    "reason": "no exchange code exists yet",
                }
            ],
        )
    }
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    assert audit(root).ok
    result = audit(root, check_deferred="M1")
    assert any("obligation due at M1 is still open" in e for e in result.errors)


def test_update_status_refuses_verified_without_audit_record(catalogue, capsys):
    statuses = {"AEGIS-001": entry("in_progress")}
    root = catalogue([requirement()], statuses)
    write(root, "tools/thing.py", "x = 1\n")
    write(root, "tests/unit/test_thing.py", "def test_thing():\n    assert True\n")
    code = update_status.main(
        [
            "AEGIS-001",
            "verified",
            "--implementation",
            "tools/thing.py",
            "--test",
            "tests/unit/test_thing.py",
            "--status-path",
            str(root / "requirements/implementation_status.json"),
            "--root",
            str(root),
        ]
    )
    assert code == 2
    assert "requires --auditor" in capsys.readouterr().err


def test_update_status_refuses_missing_evidence_path(catalogue, capsys):
    root = catalogue([requirement()], {"AEGIS-001": entry("in_progress")})
    code = update_status.main(
        [
            "AEGIS-001",
            "implemented",
            "--implementation",
            "tools/never_written.py",
            "--status-path",
            str(root / "requirements/implementation_status.json"),
            "--root",
            str(root),
        ]
    )
    assert code == 2
    assert "path does not exist" in capsys.readouterr().err


def test_update_status_writes_a_supportable_claim(catalogue):
    root = catalogue([requirement()], {"AEGIS-001": entry("in_progress")})
    write(root, "tools/thing.py", "x = 1\n")
    code = update_status.main(
        [
            "AEGIS-001",
            "implemented",
            "--implementation",
            "tools/thing.py",
            "--status-path",
            str(root / "requirements/implementation_status.json"),
            "--root",
            str(root),
        ]
    )
    assert code == 0
    assert audit(root).ok
