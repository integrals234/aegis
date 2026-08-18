"""M4 shared report foundation (AEGIS-079, AEGIS-081, AEGIS-024).

Batch 1 ships the foundation only -- report *content* is later batch work.
These tests prove the two properties the plan of record requires of it:
deterministic serialization, and that provenance tracks real input content
rather than just a path name.

`build_report_provenance` calls `git_commit`, which shells out to the real
`git` in its `root` -- so these tests use the real `repo_root` fixture and a
real committed input file, rather than a bare `tmp_path` (not a git
worktree). Content-mutation is tested against `hash_file` directly instead,
on a throwaway file, since that function has no git dependency.
"""

from __future__ import annotations

from decimal import Decimal
from pathlib import Path

import pytest
from reports.report_model import build_report_provenance, hash_file, render_report

pytestmark = pytest.mark.unit

# A small, real, already-committed input: no temp-file writes into the repo,
# and the content is stable across the run.
INPUT_RELATIVE_PATH = "data_samples/futures/bars/eqx.csv"


def _provenance(repo_root: Path, strategy_config: dict[str, object] | None = None):
    return build_report_provenance(
        report_id="test-report",
        root=repo_root,
        input_paths=[INPUT_RELATIVE_PATH],
        strategy_config=strategy_config or {"entry_threshold": 2.0, "window": 20},
        dataset_id="EQX-2026H",
        roll_policy_name="FixedDaysPolicy",
    )


def test_two_renders_of_the_same_inputs_are_byte_identical(repo_root: Path) -> None:
    provenance = _provenance(repo_root)
    findings = {"z_score": 2.1213203435596393, "spread": Decimal("0.60")}

    first = render_report(provenance, findings)
    second = render_report(provenance, findings)

    assert first == second


def test_mutating_a_file_changes_its_recorded_digest(tmp_path: Path) -> None:
    path = tmp_path / "prices.csv"
    path.write_text("near,far\n100,105\n", encoding="utf-8")
    before = hash_file(path)

    path.write_text("near,far\n999,999\n", encoding="utf-8")
    after = hash_file(path)

    assert before != after


def test_provenance_pins_the_actual_content_not_just_the_path(repo_root: Path) -> None:
    provenance = _provenance(repo_root)
    assert provenance.inputs[0].path == INPUT_RELATIVE_PATH
    assert provenance.inputs[0].content_sha256 == hash_file(repo_root / INPUT_RELATIVE_PATH)


def test_decimal_findings_serialize_exactly_via_str_not_float(repo_root: Path) -> None:
    provenance = _provenance(repo_root)
    rendered = render_report(provenance, {"hedge_ratio": Decimal("1.333333333333333333")})
    assert '"1.333333333333333333"' in rendered


def test_serialization_is_stable_key_order_regardless_of_dict_insertion_order(
    repo_root: Path,
) -> None:
    provenance = _provenance(repo_root, strategy_config={"b": 1, "a": 2})
    rendered = render_report(provenance, {"z": 1, "a": 2})
    assert rendered.index('"a"') < rendered.index('"z"')


# --- M5: sibling-evidence exclusion (the carried M4 debt) -------------------
#
# Uses a throwaway git repo rather than the real repository: the real tree's
# dirty/clean state changes as this very milestone's work proceeds, so a test
# asserting on it would be testing today's ambient state, not the exclusion
# rule itself.


def _init_throwaway_repo(tmp_path: Path) -> Path:
    import subprocess

    repo = tmp_path / "throwaway_repo"
    repo.mkdir()
    (repo / "code.py").write_text("x = 1\n")
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.name", "test"], cwd=repo, check=True)
    subprocess.run(["git", "add", "-A"], cwd=repo, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "initial"], cwd=repo, check=True)
    return repo


def test_a_sibling_evidence_artifact_does_not_mark_the_commit_dirty(tmp_path: Path) -> None:
    repo = _init_throwaway_repo(tmp_path)
    clean_commit = build_report_provenance(
        report_id="r", root=repo, input_paths=[], strategy_config={}, dataset_id="d",
        roll_policy_name="p",
    ).code_commit
    assert not clean_commit.endswith("-dirty")

    # A generator writing its own sibling artifact in the same batch -- the
    # exact scenario that produced the M4 debt.
    evidence_dir = repo / "experiments" / "evidence" / "AEGIS-999"
    evidence_dir.mkdir(parents=True)
    (evidence_dir / "output.json").write_text("{}\n")

    still_clean_commit = build_report_provenance(
        report_id="r", root=repo, input_paths=[], strategy_config={}, dataset_id="d",
        roll_policy_name="p",
    ).code_commit
    assert not still_clean_commit.endswith("-dirty")
    assert still_clean_commit == clean_commit


def test_an_actual_unrelated_code_change_still_marks_the_commit_dirty(tmp_path: Path) -> None:
    repo = _init_throwaway_repo(tmp_path)
    (repo / "code.py").write_text("x = 2\n")  # A real, non-evidence modification.

    dirty_commit = build_report_provenance(
        report_id="r", root=repo, input_paths=[], strategy_config={}, dataset_id="d",
        roll_policy_name="p",
    ).code_commit
    assert dirty_commit.endswith("-dirty")
