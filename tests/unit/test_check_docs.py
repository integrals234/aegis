"""AEGIS-235 — the documentation audit must be executable, not a checklist."""

from __future__ import annotations

import json
from pathlib import Path

import check_docs
import pytest

pytestmark = pytest.mark.unit


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    (root / "docs").mkdir(parents=True)
    (root / "requirements").mkdir(parents=True)
    (root / "requirements/requirements.json").write_text(
        json.dumps({"requirements": [{"id": "AEGIS-235", "module": "Engineering Platform", "milestone": "M0"}]}),
        encoding="utf-8",
    )
    for rel in check_docs.REQUIRED_DOCS:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"# {Path(rel).stem}\n\nReal content for the documentation audit.\n", encoding="utf-8")
    return root


def test_complete_documentation_passes(tree):
    assert check_docs.run(tree) == []


def test_missing_required_document_is_reported(tree):
    (tree / "docs/RUNBOOK.md").unlink()
    assert any("required document missing: docs/RUNBOOK.md" in e for e in check_docs.run(tree))


def test_empty_required_document_is_reported(tree):
    (tree / "docs/DEMO.md").write_text("", encoding="utf-8")
    assert any("docs/DEMO.md is empty" in e for e in check_docs.run(tree))


def test_placeholder_document_is_reported(tree):
    (tree / "docs/LIMITATIONS.md").write_text("TODO: write this\n", encoding="utf-8")
    assert any("only TODO/placeholder text" in e for e in check_docs.run(tree))


def test_unresolvable_evidence_marker_is_reported(tree):
    (tree / "docs/RUNBOOK.md").write_text(
        "# Runbook\n\nDeterminism is checked, evidence: experiments/evidence/nope.hash\n", encoding="utf-8"
    )
    assert any("evidence path does not exist" in e for e in check_docs.run(tree))


def test_resolvable_evidence_marker_passes(tree):
    (tree / "experiments/evidence").mkdir(parents=True)
    (tree / "experiments/evidence/run.hash").write_text("abc123\n", encoding="utf-8")
    (tree / "docs/RUNBOOK.md").write_text(
        "# Runbook\n\nDeterminism is checked, evidence: experiments/evidence/run.hash\n", encoding="utf-8"
    )
    assert check_docs.run(tree) == []


def test_broken_internal_link_is_reported(tree):
    (tree / "docs/DEMO.md").write_text("# Demo\n\nSee [the runbook](docs/GONE.md).\n", encoding="utf-8")
    assert any("broken link: docs/GONE.md" in e for e in check_docs.run(tree))


def test_external_link_is_not_checked(tree):
    (tree / "docs/DEMO.md").write_text(
        "# Demo\n\nSee [upstream](https://example.invalid/page).\n", encoding="utf-8"
    )
    assert check_docs.run(tree) == []


def test_unknown_requirement_reference_is_reported(tree):
    (tree / "docs/DEMO.md").write_text("# Demo\n\nCovers AEGIS-999.\n", encoding="utf-8")
    assert any("unknown requirement AEGIS-999" in e for e in check_docs.run(tree))
