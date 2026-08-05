"""AEGIS-008 — decision records must be valid and actually referenced."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

import check_adrs

pytestmark = pytest.mark.unit

VALID_ADR = """\
# ADR-0001: Platform architecture

- Status: Accepted
- Date: 2026-08-05
- Requirement IDs: AEGIS-004, AEGIS-008
- Milestone: M0

## Context

The exchange and the participant must stay separate.

## Decision

Enforce the layer DAG structurally.

## Alternatives considered

Review-only enforcement, which does not survive a busy week.

## Consequences

Layer edges must be declared before code can use them.

## Verification

tools/check_architecture.py and tests/unit/test_check_architecture.py.
"""


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=True).stdout


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    (root / "adr").mkdir(parents=True)
    (root / "requirements").mkdir(parents=True)
    (root / "requirements/requirements.json").write_text(
        json.dumps(
            {
                "requirements": [
                    {"id": "AEGIS-004", "module": "Governance", "milestone": "M0", "title": "sep"},
                    {"id": "AEGIS-008", "module": "Governance", "milestone": "M0", "title": "adr"},
                ]
            }
        ),
        encoding="utf-8",
    )
    (root / "adr/0001-platform-architecture.md").write_text(VALID_ADR, encoding="utf-8")
    return root


def run(tree: Path) -> list[str]:
    return check_adrs.run(tree, check_link=False)


def test_valid_adr_passes(tree):
    assert run(tree) == []


def test_missing_section_is_reported(tree):
    path = tree / "adr/0001-platform-architecture.md"
    path.write_text(path.read_text(encoding="utf-8").replace("## Alternatives considered", "## Notes"), encoding="utf-8")
    assert any("Alternatives considered" in e for e in run(tree))


def test_unknown_status_is_reported(tree):
    path = tree / "adr/0001-platform-architecture.md"
    path.write_text(path.read_text(encoding="utf-8").replace("Status: Accepted", "Status: Maybe"), encoding="utf-8")
    assert any("is not one of" in e for e in run(tree))


def test_unknown_requirement_id_is_reported(tree):
    path = tree / "adr/0001-platform-architecture.md"
    path.write_text(path.read_text(encoding="utf-8").replace("AEGIS-004", "AEGIS-999"), encoding="utf-8")
    assert any("cites unknown requirement AEGIS-999" in e for e in run(tree))


def test_missing_requirement_ids_is_reported(tree):
    path = tree / "adr/0001-platform-architecture.md"
    path.write_text(
        path.read_text(encoding="utf-8").replace("- Requirement IDs: AEGIS-004, AEGIS-008", "- Requirement IDs:"),
        encoding="utf-8",
    )
    assert any("must cite at least one requirement" in e for e in run(tree))


def test_missing_milestone_is_reported(tree):
    path = tree / "adr/0001-platform-architecture.md"
    path.write_text(path.read_text(encoding="utf-8").replace("- Milestone: M0\n", ""), encoding="utf-8")
    assert any("'Milestone:' must be a milestone ID" in e for e in run(tree))


def test_dangling_adr_reference_is_reported(tree):
    (tree / "docs").mkdir()
    (tree / "docs/NOTES.md").write_text("See ADR-0042 for the rationale.\n", encoding="utf-8")
    assert any("references ADR-0042, which does not exist" in e for e in run(tree))


def test_bad_filename_is_reported(tree):
    (tree / "adr/notes.md").write_text(VALID_ADR, encoding="utf-8")
    assert any("filename must be" in e for e in run(tree))


def test_architecture_change_without_an_adr_reference_is_reported(tree):
    git(tree, "init", "-q", "-b", "main")
    git(tree, "config", "user.email", "test@example.invalid")
    git(tree, "config", "user.name", "AEGIS Test")
    git(tree, "add", "-A")
    git(tree, "commit", "-qm", "baseline")
    git(tree, "checkout", "-qb", "feature")
    (tree / "cpp/common").mkdir(parents=True)
    (tree / "cpp/common/clock.hpp").write_text("#pragma once\n", encoding="utf-8")
    git(tree, "add", "-A")
    git(tree, "commit", "-qm", "add a clock with no decision record")

    errors = check_adrs.run(tree, base_ref="main", check_link=True)
    assert any("without any ADR reference" in e for e in errors)


def test_architecture_change_citing_an_adr_passes(tree):
    git(tree, "init", "-q", "-b", "main")
    git(tree, "config", "user.email", "test@example.invalid")
    git(tree, "config", "user.name", "AEGIS Test")
    git(tree, "add", "-A")
    git(tree, "commit", "-qm", "baseline")
    git(tree, "checkout", "-qb", "feature")
    (tree / "cpp/common").mkdir(parents=True)
    (tree / "cpp/common/clock.hpp").write_text("#pragma once\n", encoding="utf-8")
    git(tree, "add", "-A")
    git(tree, "commit", "-qm", "add a clock as decided in ADR-0001")

    assert check_adrs.run(tree, base_ref="main", check_link=True) == []


def test_live_adrs_are_valid(repo_root):
    errors, _ = check_adrs.check_structure(repo_root)
    assert errors == []
