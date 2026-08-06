"""AEGIS-005 — the determinism harness must detect nondeterminism.

The claim under test is deliberately narrow and matches what the milestone
report says: *the harness detects nondeterminism*. It is not "AEGIS is
deterministic" — at M0 there is no engine whose determinism could be claimed,
and the producers emit platform records only.

The negative fixture is the load-bearing test here. A harness that has only
ever compared stable output cannot demonstrate that it would notice unstable
output, so ``test_harness_flags_the_nondeterministic_producer`` is what turns
this from an assumption into a checked property.
"""

from __future__ import annotations

import determinism_check
import pytest
from common.determinism import PRODUCERS, run_producer

pytestmark = pytest.mark.replay


def test_platform_producer_is_stable_across_processes(repo_root):
    """Separate processes, different PYTHONHASHSEED: a dict-order dependency
    would surface here rather than in CI six months from now."""
    assert determinism_check.main(["--root", str(repo_root), "--runs", "3", "--seed", "42"]) == 0


def test_harness_flags_the_nondeterministic_producer(repo_root, capsys):
    """Without this, the harness could be broken and every run would still pass."""
    code = determinism_check.main(
        ["--root", str(repo_root), "--producer", "nondeterministic", "--runs", "2"]
    )
    assert code == 2
    assert "different canonical output" in capsys.readouterr().err


def test_negative_fixture_mode_requires_instability(repo_root, capsys):
    """--expect-failure must itself fail if the fixture stops being unstable."""
    assert (
        determinism_check.main(
            ["--root", str(repo_root), "--producer", "nondeterministic", "--expect-failure"]
        )
        == 0
    )

    code = determinism_check.main(
        ["--root", str(repo_root), "--producer", "platform", "--expect-failure"]
    )
    assert code == 2
    assert "no longer proving" in capsys.readouterr().err


def test_a_single_run_is_rejected(repo_root, capsys):
    """One run cannot disagree with itself, so it proves nothing."""
    assert determinism_check.main(["--root", str(repo_root), "--runs", "1"]) == 2
    assert "at least 2" in capsys.readouterr().err


def test_the_seed_changes_the_output(repo_root):
    """If it did not, the seed would not be an input and reruns would be vacuous."""
    first = run_producer("platform", 1, repo_root)
    second = run_producer("platform", 2, repo_root)
    assert first != second


def test_the_same_seed_reproduces_the_output_in_process(repo_root):
    assert run_producer("platform", 7, repo_root) == run_producer("platform", 7, repo_root)


def test_every_line_is_already_in_canonical_form(repo_root):
    """Re-serializing a record must reproduce it exactly.

    This is the property that makes a hash comparable: if any line could be
    written more than one way — different key order, different float formatting,
    incidental whitespace — then two runs could differ for reasons that have
    nothing to do with what produced them.
    """
    import json
    import math

    output = run_producer("platform", 3, repo_root)
    for line in output.splitlines():
        record = json.loads(line)
        assert json.dumps(record, sort_keys=True, separators=(",", ":")) == line

        def assert_finite(node: object) -> None:
            if isinstance(node, float):
                assert math.isfinite(node), node
            elif isinstance(node, dict):
                for value in node.values():
                    assert_finite(value)
            elif isinstance(node, list):
                for value in node:
                    assert_finite(value)

        assert_finite(record)


def test_producers_are_registered_by_name():
    assert set(PRODUCERS) == {"platform", "nondeterministic"}


def test_unknown_producer_is_reported_clearly():
    with pytest.raises(KeyError, match="registered producers are"):
        run_producer("does-not-exist", 1)


def test_evidence_records_the_narrow_claim(repo_root):
    """The committed evidence must not overstate what M0 proved."""
    import json

    summary = json.loads(
        (repo_root / "experiments/evidence/AEGIS-005/summary.json").read_text(encoding="utf-8")
    )
    assert summary["stable"] is True
    assert "not a claim that AEGIS is deterministic" in summary["claim"]
    assert len(set(summary["digests"])) == 1
