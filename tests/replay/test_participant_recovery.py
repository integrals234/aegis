"""AEGIS-237 discharge for the M3 participant core (ADR-0024).

The participant-state analogue of ``test_exchange_determinism.py``'s
``test_snapshot_continuation_equality_across_processes``: a run split at a
``ParticipantSnapshot`` boundary across two ``aegis_participant_run``
process invocations must reproduce exactly the tail of the same scenario
run uninterrupted through one process -- OMS order lifecycle and portfolio
positions surviving a real process boundary, not merely an in-process
round trip (that half of the proof is
``tests/cpp/unit/test_participant_snapshot.cpp``).

This is deliberately not AEGIS-061 (in-stream feed-gap recovery) and not
AEGIS-070 (book-snapshot re-base): neither exchange nor market-data state
is involved here at all, only OMS + portfolio state (ADR-0024's contract
boundary).
"""

from __future__ import annotations

import json
import subprocess

import pytest
from common.determinism import resolve_participant_run_binary

pytestmark = pytest.mark.replay

FIXTURE_RELATIVE_PATH = "tests/unit/fixtures/participant/recovery_scenario.jsonl"


def _run(binary, fixture, args: list[str]) -> str:
    result = subprocess.run(
        [str(binary), "--fixture", str(fixture), *args],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    return result.stdout


def test_missing_participant_run_binary_raises_not_skips(repo_root, monkeypatch, tmp_path):
    """A missing binary must fail the check, never silently skip it (matches
    AEGIS-012's rule for aegis_exchange_replay)."""
    monkeypatch.setenv("AEGIS_PARTICIPANT_RUN", str(tmp_path / "does-not-exist"))
    with pytest.raises(FileNotFoundError, match="aegis_participant_run not found"):
        resolve_participant_run_binary(repo_root)


def test_fixture_produces_a_nontrivial_scenario(repo_root):
    """Sanity check on the committed fixture itself: at least one order
    reaches a real fill and a real terminal state, not just empty/default
    state -- otherwise a broken snapshot codec could pass by accident."""
    binary = resolve_participant_run_binary(repo_root)
    fixture = repo_root / FIXTURE_RELATIVE_PATH
    lines = _run(binary, fixture, []).splitlines()
    assert len(lines) == 15  # One line per committed fixture step.

    final = json.loads(lines[-1])
    assert final["cash_units"] != 0
    assert any(position["quantity_units"] != 0 for position in final["positions"])
    assert any(order["cumulative_filled_units"] > 0 for order in final["orders"])


def test_participant_run_output_is_deterministic_across_processes(repo_root):
    """Two independent, uninterrupted invocations over the same fixture must
    produce byte-identical output (AEGIS-005's cross-process half, mirroring
    test_exchange_producer_is_stable_across_processes)."""
    binary = resolve_participant_run_binary(repo_root)
    fixture = repo_root / FIXTURE_RELATIVE_PATH
    first = _run(binary, fixture, [])
    second = _run(binary, fixture, [])
    assert first == second


def test_snapshot_continuation_equality_across_processes(repo_root, tmp_path):
    """The AEGIS-237 process-boundary proof: split the committed scenario at
    step 9 (a resting, partially-filled order and a second, already-filled
    instrument are both live at the split), snapshot, restore into a fresh
    process, and finish the remaining steps. The restored tail must be
    byte-identical to the same tail of one uninterrupted run.
    """
    binary = resolve_participant_run_binary(repo_root)
    fixture = repo_root / FIXTURE_RELATIVE_PATH

    full_output = _run(binary, fixture, [])
    full_lines = full_output.splitlines()

    snapshot_path = tmp_path / "participant_snapshot.bin"
    first_half = _run(binary, fixture, ["--limit", "9", "--snapshot-out", str(snapshot_path)])
    second_half = _run(binary, fixture, ["--skip", "9", "--restore-from", str(snapshot_path)])

    first_half_lines = first_half.splitlines()
    assert first_half_lines == full_lines[: len(first_half_lines)]

    expected_second_half_lines = full_lines[len(first_half_lines) :]
    assert expected_second_half_lines, "the split must leave a nonempty tail to compare"
    assert second_half.splitlines() == expected_second_half_lines


def test_snapshot_bytes_are_stable_across_repeated_captures(repo_root, tmp_path):
    """AEGIS-237/ADR-0024 byte stability: capturing the same state twice must
    produce byte-identical snapshot files, independent of the determinism
    check above (which compares stdout, not the snapshot file itself)."""
    binary = resolve_participant_run_binary(repo_root)
    fixture = repo_root / FIXTURE_RELATIVE_PATH

    first_path = tmp_path / "first.bin"
    second_path = tmp_path / "second.bin"
    _run(binary, fixture, ["--limit", "9", "--snapshot-out", str(first_path)])
    _run(binary, fixture, ["--limit", "9", "--snapshot-out", str(second_path)])

    assert first_path.read_bytes() == second_path.read_bytes()


def test_restore_from_a_corrupt_snapshot_fails_not_silently(repo_root, tmp_path):
    """A truncated/corrupt snapshot file must be refused deterministically,
    never silently accepted as empty or partial state."""
    binary = resolve_participant_run_binary(repo_root)
    fixture = repo_root / FIXTURE_RELATIVE_PATH

    snapshot_path = tmp_path / "participant_snapshot.bin"
    _run(binary, fixture, ["--limit", "9", "--snapshot-out", str(snapshot_path)])
    corrupt_bytes = snapshot_path.read_bytes()[:-2]
    snapshot_path.write_bytes(corrupt_bytes)

    result = subprocess.run(
        [str(binary), "--fixture", str(fixture), "--skip", "9", "--restore-from", str(snapshot_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "failed to restore participant snapshot" in result.stderr
