"""AEGIS-003 -- the provenance stamp on evidence must be truthful.

``dirty`` exists so a reader knows whether an artifact can be rebuilt from the
commit it names. These tests pin the two halves of that meaning: uncommitted
*code* must set it, and sibling evidence written by the same batch must not.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest
from evidence_provenance import code_is_dirty, provenance

pytestmark = pytest.mark.unit


def _repo(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    (root / "experiments/evidence/AEGIS-001").mkdir(parents=True)
    (root / "tools").mkdir()
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "config", "user.email", "t@example.com"], cwd=root, check=True)
    subprocess.run(["git", "config", "user.name", "t"], cwd=root, check=True)
    (root / "tools/producer.py").write_text("print('hi')\n", encoding="utf-8")
    (root / "experiments/evidence/AEGIS-001/a.json").write_text("{}\n", encoding="utf-8")
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    subprocess.run(["git", "commit", "-qm", "init"], cwd=root, check=True)
    return root


def test_clean_tree_is_not_dirty(tmp_path):
    assert code_is_dirty(_repo(tmp_path)) is False


def test_regenerated_evidence_alone_is_not_dirty(tmp_path):
    """The batch-regeneration case: writing evidence must not make the *next*
    artifact claim the code was uncommitted."""
    root = _repo(tmp_path)
    (root / "experiments/evidence/AEGIS-001/a.json").write_text('{"x": 1}\n', encoding="utf-8")
    (root / "experiments/evidence/AEGIS-001/b.json").write_text("{}\n", encoding="utf-8")
    assert code_is_dirty(root) is False


def test_uncommitted_producing_code_is_dirty(tmp_path):
    """The case the field exists to warn about."""
    root = _repo(tmp_path)
    (root / "tools/producer.py").write_text("print('changed')\n", encoding="utf-8")
    assert code_is_dirty(root) is True


def test_untracked_code_outside_evidence_is_dirty(tmp_path):
    root = _repo(tmp_path)
    (root / "tools/another.py").write_text("x = 1\n", encoding="utf-8")
    assert code_is_dirty(root) is True


def test_code_change_still_wins_over_evidence_changes(tmp_path):
    """Both kinds present at once must still report dirty."""
    root = _repo(tmp_path)
    (root / "experiments/evidence/AEGIS-001/a.json").write_text('{"x": 2}\n', encoding="utf-8")
    (root / "tools/producer.py").write_text("print('changed')\n", encoding="utf-8")
    assert code_is_dirty(root) is True


def test_provenance_block_names_the_commit_and_explains_dirty(tmp_path):
    root = _repo(tmp_path)
    block = provenance(root)
    assert len(block["repository_commit"]) == 40
    assert block["dirty"] is False
    assert "experiments/evidence" in block["dirty_means"]
