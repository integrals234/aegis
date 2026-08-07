"""AEGIS-005 discharge for the M1 exchange core (ADR-0012, ADR-0013).

Two properties, matching the plan of record: two fresh processes with
different ``PYTHONHASHSEED`` produce byte-identical canonical output from
``aegis_exchange_replay`` on the committed scenario, and a run split at a
snapshot boundary across two process invocations reproduces exactly the tail
of the same scenario run uninterrupted through one process.
"""

from __future__ import annotations

import json
import subprocess

import determinism_check
import pytest
from common.determinism import EXCHANGE_SCENARIO_RELATIVE_PATH, resolve_exchange_replay_binary, run_producer

pytestmark = pytest.mark.replay


def test_exchange_producer_is_stable_across_processes(repo_root):
    """Separate processes, different PYTHONHASHSEED (determinism_check.py's own
    mechanism): a dependency on the exchange producer's subprocess environment
    would surface here rather than as a later replay mismatch."""
    assert (
        determinism_check.main(["--root", str(repo_root), "--producer", "exchange", "--runs", "3"])
        == 0
    )


def test_exchange_producer_output_is_nonempty_canonical_hex(repo_root):
    output = run_producer("exchange", 1, repo_root)
    lines = output.splitlines()
    assert lines, "the committed scenario must emit at least one event"
    for line in lines:
        # Canonical form (cpp/events/envelope.hpp): lowercase hex, even length.
        assert line == line.lower()
        assert len(line) % 2 == 0
        int(line, 16)  # raises ValueError if not valid hex


def test_exchange_producer_ignores_seed(repo_root):
    """The engine reads no random source (ADR-0012); seed only satisfies the
    common producer signature, and does not vary the output once root is
    fixed."""
    assert run_producer("exchange", 1, repo_root) == run_producer("exchange", 99, repo_root)


def test_missing_replay_binary_raises_not_skips(repo_root, monkeypatch, tmp_path):
    """A missing binary must fail the check, never silently skip it (ADR-0012)."""
    monkeypatch.setenv("AEGIS_EXCHANGE_REPLAY", str(tmp_path / "does-not-exist"))
    with pytest.raises(FileNotFoundError, match="aegis_exchange_replay not found"):
        resolve_exchange_replay_binary(repo_root)


def test_snapshot_continuation_equality_across_processes(repo_root, tmp_path):
    """A run split as first half -> snapshot -> restore into a fresh process ->
    second half must emit byte-identical canonical output, for the split's
    tail, to the same scenario run uninterrupted through one process
    (ADR-0013's acceptance criterion, exercised here as a process boundary
    rather than in-process as tests/cpp/unit/test_snapshot_roundtrip.cpp does).
    """
    binary = resolve_exchange_replay_binary(repo_root)
    fixture = repo_root / EXCHANGE_SCENARIO_RELATIVE_PATH

    def run(args: list[str]) -> str:
        result = subprocess.run(
            [str(binary), "--input", str(fixture), *args],
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode == 0, result.stderr
        return result.stdout

    full_output = run([])
    full_lines = full_output.splitlines()

    snapshot_path = tmp_path / "exchange_replay_snapshot.bin"
    first_half = run(["--limit", "6", "--snapshot-out", str(snapshot_path)])
    second_half = run(["--skip", "6", "--restore-from", str(snapshot_path)])

    first_half_lines = first_half.splitlines()
    expected_second_half_lines = full_lines[len(first_half_lines) :]

    assert expected_second_half_lines, "the split must leave a nonempty tail to compare"
    assert second_half.splitlines() == expected_second_half_lines


def test_exchange_evidence_claim_is_not_the_m0_platform_wording(repo_root, tmp_path):
    """ADR-0012: the M0 platform claim text ('not a claim that AEGIS is
    deterministic') must not travel onto exchange evidence, which is exactly
    that claim."""
    evidence_dir = tmp_path / "evidence"
    code = determinism_check.main(
        [
            "--root",
            str(repo_root),
            "--producer",
            "exchange",
            "--runs",
            "2",
            "--write-evidence",
            str(evidence_dir),
        ]
    )
    assert code == 0
    summary = json.loads((evidence_dir / "summary.json").read_text(encoding="utf-8"))
    assert "not a claim that AEGIS is deterministic" not in summary["claim"]
    assert "AEGIS-005" in summary["claim"]
    assert summary["stable"] is True
