"""AEGIS-002 — the requirement auditor must detect catalogue integrity failures.

Each test drives the auditor over a fixture tree, so a passing test proves the
gate fires rather than proving the current repository happens to be clean.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from audit_requirements import DuplicateKeyError, load_json, run_audit

pytestmark = pytest.mark.unit


def requirement(rid: str, milestone: str = "M0", module: str = "Governance", priority: str = "must") -> dict:
    return {
        "id": rid,
        "module": module,
        "milestone": milestone,
        "priority": priority,
        "title": f"title {rid}",
        "description": f"description {rid}",
        "acceptance": f"acceptance {rid}",
    }


def status(value: str = "not_started", **extra: object) -> dict:
    entry = {"status": value, "implementation": [], "tests": [], "reports": []}
    entry.update(extra)
    return entry


def audit(root: Path, **kwargs: object):
    return run_audit(
        req_path=root / "requirements/requirements.json",
        status_path=root / "requirements/implementation_status.json",
        root=root,
        **kwargs,  # type: ignore[arg-type]
    )


def test_clean_catalogue_passes(catalogue):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status()})
    result = audit(root, milestone="M0")
    assert result.ok, result.errors
    assert result.counts == {"not_started": 1}


def test_duplicate_requirement_ids_are_reported(catalogue):
    root = catalogue(
        [requirement("AEGIS-001"), requirement("AEGIS-001")],
        {"AEGIS-001": status()},
    )
    result = audit(root)
    assert any("duplicate requirement IDs" in e for e in result.errors)


def test_missing_status_entry_is_reported(catalogue):
    root = catalogue([requirement("AEGIS-001"), requirement("AEGIS-002")], {"AEGIS-001": status()})
    result = audit(root)
    assert any("missing status entry: AEGIS-002" in e for e in result.errors)


def test_orphan_status_entry_is_reported(catalogue):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status(), "AEGIS-999": status()})
    result = audit(root)
    assert any("orphan status entry: AEGIS-999" in e for e in result.errors)


def test_absent_required_module_is_reported(catalogue, tmp_path):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status()})
    doc = json.loads((root / "requirements/requirements.json").read_text())
    doc["requirements"] = [r for r in doc["requirements"] if r["module"] != "Online Statistics"]
    (root / "requirements/requirements.json").write_text(json.dumps(doc), encoding="utf-8")
    result = audit(root)
    assert any("required module absent: Online Statistics" in e for e in result.errors)


def test_invalid_status_value_is_reported(catalogue):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status("done")})
    result = audit(root)
    assert any("invalid status 'done'" in e for e in result.errors)


def test_unknown_milestone_is_reported(catalogue):
    root = catalogue([requirement("AEGIS-001", milestone="M42")], {"AEGIS-001": status()})
    result = audit(root)
    assert any("unknown milestone 'M42'" in e for e in result.errors)


def test_must_requirement_cannot_be_deferred(catalogue):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status("deferred")})
    result = audit(root)
    assert any("cannot be silently deferred" in e for e in result.errors)


def test_unknown_status_field_is_reported(catalogue):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status(completed=True)})
    result = audit(root)
    assert any("unknown status fields: completed" in e for e in result.errors)


def test_duplicate_json_key_is_rejected(tmp_path):
    path = tmp_path / "dupe.json"
    path.write_text('{"requirements": [], "requirements": []}', encoding="utf-8")
    with pytest.raises(DuplicateKeyError):
        load_json(path)


def test_duplicate_json_key_fails_the_audit(catalogue):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status()})
    path = root / "requirements/implementation_status.json"
    path.write_text(
        '{"requirements": {"AEGIS-001": {"status": "not_started"}, "AEGIS-001": {"status": "verified"}}}',
        encoding="utf-8",
    )
    result = audit(root)
    assert any("duplicate JSON key" in e for e in result.errors)


def test_milestone_filter_restricts_scope(catalogue):
    root = catalogue(
        [requirement("AEGIS-001", milestone="M0"), requirement("AEGIS-002", milestone="M1")],
        {"AEGIS-001": status(), "AEGIS-002": status("done")},
    )
    assert audit(root, milestone="M0").ok
    assert not audit(root, milestone="M1").ok


def test_unknown_milestone_filter_is_an_error(catalogue):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status()})
    result = audit(root, milestone="M5")
    assert any("no requirements found for milestone M5" in e for e in result.errors)


def test_frozen_file_mutation_is_detected(catalogue, tmp_path):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status()})
    frozen = root / "docs/SPEC.md"
    frozen.parent.mkdir(parents=True, exist_ok=True)
    frozen.write_text("original\n", encoding="utf-8")
    import hashlib

    digest = hashlib.sha256(frozen.read_bytes()).hexdigest()
    (root / "requirements/frozen_hashes.json").write_text(
        json.dumps({"docs/SPEC.md": digest}), encoding="utf-8"
    )
    assert audit(root).ok

    frozen.write_text("tampered\n", encoding="utf-8")
    result = audit(root)
    assert any("frozen file changed without version update" in e for e in result.errors)


def test_missing_frozen_file_is_detected(catalogue):
    root = catalogue([requirement("AEGIS-001")], {"AEGIS-001": status()}, frozen={"docs/GONE.md": "0" * 64})
    result = audit(root)
    assert any("frozen file missing: docs/GONE.md" in e for e in result.errors)


def test_live_repository_audits_clean(repo_root):
    """The gate must also hold for the real catalogue, not only for fixtures."""
    result = run_audit(
        req_path=repo_root / "requirements/requirements.json",
        status_path=repo_root / "requirements/implementation_status.json",
        root=repo_root,
    )
    assert result.ok, result.errors
    assert result.checked == 238
