#!/usr/bin/env python3
"""Generate AEGIS-237 evidence: participant-state (OMS + portfolio)
process-boundary recovery, discharging the M3 portion of the inherited
obligation (ADR-0024).

Every figure here comes from running the actual `aegis_participant_run`
binary over the committed deterministic recovery scenario
(`tests/unit/fixtures/participant/recovery_scenario.jsonl`) -- the same
comparisons `tests/replay/test_participant_recovery.py` makes as pytest
assertions, run here to produce a committed artifact instead.

Regenerate with: python3 tools/generate_participant_recovery_evidence.py
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from common.determinism import resolve_participant_run_binary
from evidence_provenance import provenance

FIXTURE_RELATIVE_PATH = "tests/unit/fixtures/participant/recovery_scenario.jsonl"
SPLIT_STEP = 9
OUTPUT_PATH = ROOT / "experiments/evidence/AEGIS-237/participant_recovery.json"


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _run(binary: Path, fixture: Path, args: list[str]) -> str:
    result = subprocess.run(
        [str(binary), "--fixture", str(fixture), *args],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def main() -> int:
    binary = resolve_participant_run_binary(ROOT)
    fixture = ROOT / FIXTURE_RELATIVE_PATH

    full_output = _run(binary, fixture, [])
    full_lines = full_output.splitlines()

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        snapshot_a = tmp_path / "snapshot_a.bin"
        snapshot_b = tmp_path / "snapshot_b.bin"

        first_half = _run(
            binary, fixture, ["--limit", str(SPLIT_STEP), "--snapshot-out", str(snapshot_a)]
        )
        second_half = _run(
            binary, fixture, ["--skip", str(SPLIT_STEP), "--restore-from", str(snapshot_a)]
        )
        # A second, independent capture at the same boundary, for the byte-
        # stability claim (same state -> same bytes, not just "restore worked").
        _run(binary, fixture, ["--limit", str(SPLIT_STEP), "--snapshot-out", str(snapshot_b)])

        snapshot_a_bytes = snapshot_a.read_bytes()
        snapshot_b_bytes = snapshot_b.read_bytes()

        corrupt_path = tmp_path / "corrupt.bin"
        corrupt_path.write_bytes(snapshot_a_bytes[:-2])
        rejection = subprocess.run(
            [str(binary), "--fixture", str(fixture), "--skip", str(SPLIT_STEP),
             "--restore-from", str(corrupt_path)],
            capture_output=True,
            text=True,
            check=False,
        )

    first_half_lines = first_half.splitlines()
    expected_tail = full_lines[len(first_half_lines):]
    continuation_matches = second_half.splitlines() == expected_tail
    first_half_matches = first_half_lines == full_lines[: len(first_half_lines)]
    byte_stable = snapshot_a_bytes == snapshot_b_bytes
    corrupt_rejected = (
        rejection.returncode != 0
        and "failed to restore participant snapshot" in rejection.stderr
    )

    final_state = json.loads(full_lines[-1])

    result = {
        "artifact": "AEGIS-237/participant_recovery.json",
        "producer": "tools/generate_participant_recovery_evidence.py",
        "requirements": ["AEGIS-237"],
        "claim": (
            "The M3 portion of AEGIS-237 (OMS order lifecycle and portfolio "
            "positions surviving a snapshot/restore cycle) is discharged: "
            "ParticipantSnapshot v1 round-trips byte-stably, and a run split "
            "at a snapshot boundary across two aegis_participant_run process "
            "invocations reproduces the tail of the same scenario run "
            "uninterrupted through one process, for a scenario containing a "
            "partially filled order, a fully filled order on a second "
            "instrument, a rejected order and a cancellation, with a fresh "
            "order submitted and filled after restore."
        ),
        "not_evidence_for": [
            "M9 paper/live restart recovery (AEGIS-221, AEGIS-222) -- no "
            "broker/paper adapter, no session/reconnect logic, no external "
            "account reconciliation is exercised here.",
            "AEGIS-061 in-stream feed-gap recovery -- a different mechanism "
            "(a market-data wire message re-basing the book) for a different "
            "failure mode; no feed or book state is involved in this artifact.",
            "AEGIS-070 book-snapshot re-base -- no market-data or book-"
            "builder state is covered by ParticipantSnapshot (ADR-0024).",
            "Combined exchange+participant atomic recovery -- the exchange "
            "and participant snapshot mechanisms are independent; this "
            "artifact covers only the participant side.",
        ],
        "methodology": {
            "binary": str(binary.relative_to(ROOT)),
            "fixture": FIXTURE_RELATIVE_PATH,
            "fixture_step_count": len(full_lines),
            "split_step": SPLIT_STEP,
            "scenario_description": (
                "Order 1 (buy 100 @ 1000) is accepted then partially filled "
                "(40 units) before the split; order 2 (rejected on price) and "
                "order 3 (sell 30 on a second instrument, fully filled) also "
                "land before the split. After restore: order 1 is cancelled "
                "(60 units unfilled), and a fresh order 4 is submitted, "
                "accepted and fully filled -- proving next-fill continuation "
                "after restore, not just that restore did not throw."
            ),
        },
        "results": {
            "full_run_line_count": len(full_lines),
            "full_run_sha256": _sha256(full_output.encode()),
            "first_half_matches_full_run_prefix": first_half_matches,
            "restored_continuation_equals_uninterrupted_tail": continuation_matches,
            "snapshot_byte_stable_across_repeated_captures": byte_stable,
            "snapshot_a_sha256": _sha256(snapshot_a_bytes),
            "snapshot_b_sha256": _sha256(snapshot_b_bytes),
            "corrupt_snapshot_deterministically_rejected": corrupt_rejected,
            "final_state_nonzero_cash_units": final_state["cash_units"] != 0,
            "final_state_has_nonzero_position": any(
                position["quantity_units"] != 0 for position in final_state["positions"]
            ),
            "final_state_has_filled_order": any(
                order["cumulative_filled_units"] > 0 for order in final_state["orders"]
            ),
        },
        **provenance(ROOT),
    }

    all_claims_hold = (
        first_half_matches
        and continuation_matches
        and byte_stable
        and corrupt_rejected
        and result["results"]["final_state_nonzero_cash_units"]
        and result["results"]["final_state_has_nonzero_position"]
        and result["results"]["final_state_has_filled_order"]
    )
    result["all_claims_hold"] = all_claims_hold

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(f"wrote {OUTPUT_PATH.relative_to(ROOT)} ({OUTPUT_PATH.stat().st_size} bytes)")

    return 0 if all_claims_hold else 1


if __name__ == "__main__":
    raise SystemExit(main())
