"""AEGIS-006 — the claims audit must catch unsupported numbers and banned phrasing."""

from __future__ import annotations

from pathlib import Path

import check_claims
import pytest

pytestmark = pytest.mark.unit

FIXTURES = Path(__file__).parent / "fixtures"
BAD = FIXTURES / "claims_bad"
OK = FIXTURES / "claims_ok"


def claims_for(tree: Path) -> list[check_claims.Claim]:
    return check_claims.run(tree, tree / "claims_policy.yaml")


def test_clean_fixture_passes():
    """A noisy claims gate gets switched off, so honest prose must pass."""
    assert claims_for(OK) == []


def test_throughput_claim_without_evidence_is_rejected():
    assert any("messages/second" in c.text for c in claims_for(BAD))


def test_latency_claim_without_evidence_is_rejected():
    assert any("350 ns" in c.text for c in claims_for(BAD))


def test_sharpe_claim_without_evidence_is_rejected():
    assert any("Sharpe" in c.text for c in claims_for(BAD))


def test_interview_guarantee_is_rejected():
    assert any("guarantees an interview" in c.text for c in claims_for(BAD))


def test_production_and_institutional_phrasing_is_rejected():
    texts = [c.text for c in claims_for(BAD)]
    assert any("institutional-grade" in t for t in texts)
    assert any("production trading system" in t for t in texts)


def test_evidence_marker_must_resolve(tmp_path):
    """An evidence path that does not exist is not evidence."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "claims_policy.yaml").write_text(
        (OK / "claims_policy.yaml").read_text(encoding="utf-8"), encoding="utf-8"
    )
    (tree / "CLAIMS.md").write_text(
        "Median latency is 350 ns, evidence: benchmarks/does_not_exist.json\n", encoding="utf-8"
    )
    assert len(claims_for(tree)) == 1

    (tree / "benchmarks").mkdir()
    (tree / "benchmarks/does_not_exist.json").write_text("{}\n", encoding="utf-8")
    assert claims_for(tree) == []


def test_code_blocks_are_not_claims(tmp_path):
    """A command that prints a number is not a claim about AEGIS."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "claims_policy.yaml").write_text(
        (OK / "claims_policy.yaml").read_text(encoding="utf-8"), encoding="utf-8"
    )
    (tree / "CLAIMS.md").write_text(
        "Run the benchmark:\n\n```bash\naegis-bench --target-ns 350 --report p99\n```\n",
        encoding="utf-8",
    )
    assert claims_for(tree) == []


def test_exclusion_requires_a_reason(tmp_path):
    policy = tmp_path / "policy.yaml"
    policy.write_text(
        "scanned_globs: ['*.md']\n"
        "excluded_paths:\n  - path: docs/X.md\n"
        "forbidden_phrases: []\nclaim_units: [ns]\nevidence_marker: 'evidence:'\n",
        encoding="utf-8",
    )
    with pytest.raises(SystemExit):
        check_claims.load_policy(policy)


def test_phrase_scoped_exclusion_still_checks_numbers(tmp_path):
    """A file exempt from one phrase is not exempt from everything."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "policy.yaml").write_text(
        "scanned_globs: ['*.md']\n"
        "excluded_paths:\n"
        "  - path: PLAN.md\n"
        "    reason: quotes the banned vocabulary in order to ban it\n"
        "    phrases: [institutional-grade]\n"
        "forbidden_phrases: [institutional-grade, guaranteed profit]\n"
        "claim_units: [ns]\n"
        "evidence_marker: 'evidence:'\n",
        encoding="utf-8",
    )
    (tree / "PLAN.md").write_text(
        'Do not write "institutional-grade" in AEGIS prose.\n\nLatency is 350 ns.\n', encoding="utf-8"
    )
    claims = check_claims.run(tree, tree / "policy.yaml")
    assert len(claims) == 1
    assert "350 ns" in claims[0].text


def test_live_repository_has_no_unsupported_claims(repo_root):
    assert check_claims.run(repo_root, repo_root / "configs/claims_policy.yaml") == []
