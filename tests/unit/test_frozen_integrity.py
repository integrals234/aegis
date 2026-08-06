"""AEGIS-001 — frozen specification files must be tamper-evident.

The mutation tests operate on a copy of the tree in a temporary directory. The
real frozen files are never written to: a test that proves tamper detection by
tampering with the artifact it protects is not a test worth having.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
from pathlib import Path

import check_frozen
import pytest

pytestmark = pytest.mark.unit


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=True).stdout


@pytest.fixture
def frozen_repo(tmp_path: Path) -> Path:
    """A miniature repository with one frozen file, on a branch off `main`."""
    root = tmp_path / "repo"
    (root / "docs").mkdir(parents=True)
    (root / "requirements").mkdir(parents=True)
    spec = root / "docs/SPEC.md"
    spec.write_text("# Canonical specification\n\nFrozen contract.\n", encoding="utf-8")
    digest = hashlib.sha256(spec.read_bytes()).hexdigest()
    (root / "requirements/frozen_hashes.json").write_text(
        json.dumps({"docs/SPEC.md": digest}, indent=2), encoding="utf-8"
    )
    (root / "docs/BUILD_STATE.md").write_text(
        "# Build State\n\n- Active milestone: M0\n- Owner-approved scope changes: none\n",
        encoding="utf-8",
    )
    git(root, "init", "-q", "-b", "main")
    git(root, "config", "user.email", "test@example.invalid")
    git(root, "config", "user.name", "AEGIS Test")
    git(root, "add", "-A")
    git(root, "commit", "-qm", "frozen baseline")
    git(root, "checkout", "-qb", "milestone/test")
    return root


def test_intact_tree_passes(frozen_repo):
    assert check_frozen.main(["--root", str(frozen_repo), "--base", "main"]) == 0


def test_content_mutation_is_detected(frozen_repo, capsys):
    (frozen_repo / "docs/SPEC.md").write_text("# Canonical specification\n\nEdited.\n", encoding="utf-8")
    assert check_frozen.main(["--root", str(frozen_repo), "--base", "main", "--no-history"]) == 2
    assert "frozen file content changed" in capsys.readouterr().err


def test_missing_frozen_file_is_detected(frozen_repo, capsys):
    (frozen_repo / "docs/SPEC.md").unlink()
    assert check_frozen.main(["--root", str(frozen_repo), "--base", "main", "--no-history"]) == 2
    assert "frozen file missing" in capsys.readouterr().err


def test_branch_diff_touching_a_frozen_path_fails(frozen_repo, capsys):
    """Even a change that re-freezes the hash must not pass silently."""
    spec = frozen_repo / "docs/SPEC.md"
    spec.write_text("# Canonical specification\n\nEdited on the branch.\n", encoding="utf-8")
    digest = hashlib.sha256(spec.read_bytes()).hexdigest()
    (frozen_repo / "requirements/frozen_hashes.json").write_text(
        json.dumps({"docs/SPEC.md": digest}, indent=2), encoding="utf-8"
    )
    git(frozen_repo, "add", "-A")
    git(frozen_repo, "commit", "-qm", "edit spec and re-freeze")

    assert check_frozen.main(["--root", str(frozen_repo), "--base", "main"]) == 2
    err = capsys.readouterr().err
    assert "docs/SPEC.md" in err
    assert "without owner approval" in err


def test_recorded_owner_approval_permits_the_change(frozen_repo, capsys):
    spec = frozen_repo / "docs/SPEC.md"
    spec.write_text("# Canonical specification\n\nOwner revision.\n", encoding="utf-8")
    digest = hashlib.sha256(spec.read_bytes()).hexdigest()
    (frozen_repo / "requirements/frozen_hashes.json").write_text(
        json.dumps({"docs/SPEC.md": digest}, indent=2), encoding="utf-8"
    )
    (frozen_repo / "docs/BUILD_STATE.md").write_text(
        "# Build State\n\n- Active milestone: M0\n"
        "- Owner-approved scope changes: docs/SPEC.md, requirements/frozen_hashes.json\n",
        encoding="utf-8",
    )
    git(frozen_repo, "add", "-A")
    git(frozen_repo, "commit", "-qm", "owner revision")

    assert check_frozen.main(["--root", str(frozen_repo), "--base", "main"]) == 0
    assert "under recorded owner approval" in capsys.readouterr().out


def test_rewriting_the_manifest_alone_is_detected(frozen_repo, capsys):
    """The tamper detector is itself protected, or it detects nothing."""
    (frozen_repo / "requirements/frozen_hashes.json").write_text(
        json.dumps({"docs/SPEC.md": "0" * 64}, indent=2), encoding="utf-8"
    )
    git(frozen_repo, "add", "-A")
    git(frozen_repo, "commit", "-qm", "rewrite manifest")
    assert check_frozen.main(["--root", str(frozen_repo), "--base", "main"]) == 2
    err = capsys.readouterr().err
    assert "requirements/frozen_hashes.json" in err


def test_live_repository_frozen_files_are_intact(repo_root, tmp_path):
    """The five real frozen files still hash to their recorded digests."""
    manifest = check_frozen.frozen_paths(repo_root)
    assert set(manifest) == {
        "docs/MASTER_SPEC.md",
        "requirements/requirements.json",
        "docs/BENCHMARK_POLICY.md",
        "docs/CV_CLAIMS_POLICY.md",
        "docs/DATA_AND_RESEARCH_POLICY.md",
    }
    assert check_frozen.check_content(repo_root, manifest) == []


def test_mutation_of_a_copied_frozen_file_is_detected(repo_root, tmp_path):
    """Same check, run against a scratch copy that is actually tampered with."""
    scratch = tmp_path / "copy"
    shutil.copytree(repo_root / "docs", scratch / "docs")
    shutil.copytree(repo_root / "requirements", scratch / "requirements")
    manifest = check_frozen.frozen_paths(scratch)
    assert check_frozen.check_content(scratch, manifest) == []

    target = scratch / "docs/MASTER_SPEC.md"
    target.write_text(target.read_text(encoding="utf-8") + "\nsneaky addition\n", encoding="utf-8")
    errors = check_frozen.check_content(scratch, manifest)
    assert any("docs/MASTER_SPEC.md" in e for e in errors)
