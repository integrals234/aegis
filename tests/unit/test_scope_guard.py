"""AEGIS-007 — one active milestone, and edits confined to its scope.

Both halves of the acceptance criterion are tested: that BUILD_STATE names
exactly one active milestone, and that out-of-scope edits are detected unless
documented as an owner-approved exception.
"""

from __future__ import annotations

from pathlib import Path

import check_scope
import pytest

pytestmark = pytest.mark.unit

SCOPE = Path(__file__).resolve().parents[2] / "configs/milestone_scope.yaml"


def build_state(root: Path, active: str = "M0", approvals: str = "none") -> None:
    (root / "docs").mkdir(parents=True, exist_ok=True)
    (root / "docs/BUILD_STATE.md").write_text(
        "# Build State\n\n"
        f"- Active milestone: {active}\n"
        f"- Owner-approved scope changes: {approvals}\n",
        encoding="utf-8",
    )


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    root.mkdir()
    build_state(root)
    return root


def test_in_scope_paths_pass(tree):
    changed = ["tools/check_scope.py", "docs/ARCHITECTURE.md", "cpp/common/clock.hpp", "tests/unit/test_x.py"]
    assert check_scope.check(tree, SCOPE, "main", paths=changed) == []


def test_later_milestone_path_is_denied(tree):
    """Exchange code appearing during M0 is the failure this gate exists for."""
    errors = check_scope.check(tree, SCOPE, "main", paths=["cpp/exchange/matching/engine.cpp"])
    assert any("belongs to a later milestone" in e for e in errors)


def test_dashboard_during_m0_is_denied(tree):
    errors = check_scope.check(tree, SCOPE, "main", paths=["dashboard/app.py"])
    assert any("denied while M0 is active" in e for e in errors)


def test_unlisted_path_is_out_of_scope(tree):
    errors = check_scope.check(tree, SCOPE, "main", paths=["unexpected/thing.txt"])
    assert any("outside the permitted scope of M0" in e for e in errors)


def test_owner_approval_permits_an_exception(tree):
    build_state(tree, "M0", "cpp/exchange/matching/engine.cpp")
    assert check_scope.check(tree, SCOPE, "main", paths=["cpp/exchange/matching/engine.cpp"]) == []


def test_scope_follows_the_active_milestone(tree):
    build_state(tree, "M1")
    assert check_scope.check(tree, SCOPE, "main", paths=["cpp/exchange/matching/engine.cpp"]) == []
    errors = check_scope.check(tree, SCOPE, "main", paths=["dashboard/app.py"])
    assert any("denied while M1 is active" in e for e in errors)


def test_two_active_milestones_is_an_error(tree):
    (tree / "docs/BUILD_STATE.md").write_text(
        "# Build State\n\n- Active milestone: M0\n- Active milestone: M1\n", encoding="utf-8"
    )
    errors = check_scope.check(tree, SCOPE, "main", paths=[])
    assert any("more than one active milestone" in e for e in errors)


def test_missing_active_milestone_is_an_error(tree):
    (tree / "docs/BUILD_STATE.md").write_text("# Build State\n\nnothing here\n", encoding="utf-8")
    errors = check_scope.check(tree, SCOPE, "main", paths=[])
    assert any("does not name an active milestone" in e for e in errors)


def test_malformed_milestone_id_is_an_error(tree):
    build_state(tree, "milestone zero")
    errors = check_scope.check(tree, SCOPE, "main", paths=[])
    assert any("not a milestone ID" in e for e in errors)


def test_live_branch_is_in_scope(repo_root):
    # "origin/main", not bare "main": a checkout produced by actions/checkout
    # (fetch-depth 0) creates a local branch only for the ref under test, so
    # bare "main" does not resolve there even though origin/main does. This
    # is exactly the ref .github/workflows/ci.yml's own gate invocations use
    # (`--base origin/main`) for the identical reason.
    assert check_scope.check(repo_root, repo_root / "configs/milestone_scope.yaml", "origin/main") == []
