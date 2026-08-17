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
