"""AEGIS-058 cross-process discharge for the M2 replay core (ADR-0018, ADR-0019).

Slices 9-13 proved the replay core deterministic *in-process*: two calls inside
one gtest binary. This repository already treats that as insufficient evidence
everywhere else -- ``tools/determinism_check.py`` spawns a fresh interpreter per
run precisely because "running twice in one process shares module state,
interned strings and a single PYTHONHASHSEED, which hides exactly the class of
nondeterminism that later bites". AEGIS-058's frozen acceptance is "repeated
runs produce identical outputs"; these tests are what make *runs* mean separate
OS processes for the replay core too.

Everything here drives the real ``aegis_replay_run`` binary over the committed
canonical stream. Nothing reimplements ordering, pacing or fault logic.
"""

from __future__ import annotations

import json
import subprocess

import determinism_check
import pytest
from common.determinism import (
    FUTURES_REPLAY_STREAM_LABEL,
    FUTURES_REPLAY_STREAM_RELATIVE_PATH,
    resolve_replay_run_binary,
    run_producer,
)

pytestmark = pytest.mark.replay


def _run(repo_root, *args: str) -> list[dict]:
    """One CLI invocation, parsed into its JSON-Lines records."""
    binary = resolve_replay_run_binary(repo_root)
    result = subprocess.run(
        [
            str(binary),
            "--input", str(repo_root / FUTURES_REPLAY_STREAM_RELATIVE_PATH),
            "--label", FUTURES_REPLAY_STREAM_LABEL,
            *args,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    return [json.loads(line) for line in result.stdout.splitlines()]


def _record_indices(lines: list[dict]) -> list[int]:
    return [line["record_index"] for line in lines if line["type"] == "event"]


def test_producer_is_stable_across_processes(repo_root):
    """Three fresh processes, different PYTHONHASHSEED, byte-identical output."""
    assert (
        determinism_check.main(
            ["--root", str(repo_root), "--producer", "futures_replay", "--runs", "3"]
        )
        == 0
    )


def test_committed_stream_fixture_matches_production_ingestion(repo_root):
    """The fixture is derived, so it must not drift from the code deriving it.

    Without this, the committed stream could silently stop reflecting what
    `futures.ingest` actually produces, and every determinism run above would
    keep passing against a stale file.
    """
    import sys

    sys.path.insert(0, str(repo_root / "tools"))
    from make_replay_fixture import build_stream

    committed = (repo_root / FUTURES_REPLAY_STREAM_RELATIVE_PATH).read_text(encoding="utf-8")
    assert committed == build_stream(repo_root), (
        "the committed canonical stream no longer matches production ingestion; "
        "regenerate it with 'python3 tools/make_replay_fixture.py'"
    )


def test_record_index_is_replayed_as_persisted_never_recomputed(repo_root):
    """`record_index` is ingestion's output and replay's input (ADR-0018)."""
    committed = [
        json.loads(line)
        for line in (repo_root / FUTURES_REPLAY_STREAM_RELATIVE_PATH)
        .read_text(encoding="utf-8")
        .splitlines()
    ]
    emitted = _run(repo_root, "--pacing", "original")
    assert _record_indices(emitted) == [record["record_index"] for record in committed]


@pytest.mark.parametrize(
    "pacing_args",
    [
        pytest.param(["--pacing", "original"], id="original"),
        pytest.param(["--pacing", "accelerated", "--multiplier", "4"], id="accelerated"),
        pytest.param(["--pacing", "fixed", "--interval-ns", "1000"], id="fixed-rate"),
        pytest.param(["--pacing", "step"], id="step"),
    ],
)
def test_every_pacing_mode_emits_the_identical_canonical_sequence(repo_root, pacing_args):
    """AEGIS-054..057 change *when* a caller would act, never *what* is emitted
    or in what order -- asserted across processes, not argued from the code."""
    baseline = _record_indices(_run(repo_root, "--pacing", "original"))
    assert _record_indices(_run(repo_root, *pacing_args)) == baseline


def test_accelerated_pacing_scales_the_original_waits(repo_root):
    """The waits must actually differ per mode, or the test above would pass on
    a build where pacing was never applied at all."""
    original = [line["wait_nanos"] for line in _run(repo_root, "--pacing", "original") if line["type"] == "event"]
    accelerated = [
        line["wait_nanos"]
        for line in _run(repo_root, "--pacing", "accelerated", "--multiplier", "2")
        if line["type"] == "event"
    ]
    assert original != accelerated
    assert accelerated == [wait // 2 for wait in original]


def test_fixed_rate_pacing_ignores_the_original_gaps(repo_root):
    events = [line for line in _run(repo_root, "--pacing", "fixed", "--interval-ns", "1000") if line["type"] == "event"]
    # The first emission has no predecessor, so its wait is zero by design.
    assert [line["wait_nanos"] for line in events[1:]] == [1000] * (len(events) - 1)


def test_resume_reproduces_the_tail_of_an_uninterrupted_run(repo_root):
    """Cursor/resume across a *process* boundary, mirroring the exchange core's
    own snapshot-continuation proof (tests/replay/test_exchange_determinism.py)."""
    full = _run(repo_root, "--pacing", "step")
    full_indices = _record_indices(full)
    resume_after = full_indices[len(full_indices) // 2]

    resumed = _record_indices(_run(repo_root, "--pacing", "step", "--resume-after", str(resume_after)))
    expected_tail = full_indices[full_indices.index(resume_after) + 1 :]

    assert expected_tail, "the split must leave a nonempty tail to compare"
    assert resumed == expected_tail


def test_faults_are_annotated_dropped_and_fully_accounted_for(repo_root):
    """AEGIS-060/061: a missing record is genuinely absent from the stream and
    genuinely present in the accounting -- never silently lost (ADR-0019)."""
    clean = _run(repo_root, "--pacing", "step")
    faulted = _run(repo_root, "--pacing", "step", "--fault", "3:delayed:500", "--fault", "5:missing")

    manifest = faulted[0]
    assert manifest["type"] == "manifest"
    assert manifest["faults_applied"] == 2
    assert manifest["records_dropped"] == 1

    delayed = [line for line in faulted if line["type"] == "event" and line["fault"] == "delayed"]
    assert [line["record_index"] for line in delayed] == [3]

    dropped = [line for line in faulted if line["type"] == "dropped"]
    assert [line["record_index"] for line in dropped] == [5]
    assert dropped[0]["reason"] == "missing"

    # Emitted + dropped reconstructs the original set exactly.
    assert sorted(_record_indices(faulted) + [line["record_index"] for line in dropped]) == sorted(
        _record_indices(clean)
    )


def test_faults_never_perturb_the_canonical_order_of_survivors(repo_root):
    """ADR-0019's central invariant, checked end to end."""
    clean = _record_indices(_run(repo_root, "--pacing", "step"))
    faulted = _record_indices(
        _run(repo_root, "--pacing", "step", "--fault", "2:sequence_gap:0:9", "--fault", "7:spread_widening:0:3")
    )
    assert faulted == clean


def test_fixed_rate_reports_a_rate_control_metric(repo_root):
    """AEGIS-056's acceptance names rate-control tests *and metrics*.

    The metric is computed from scheduled virtual time, never a wall clock --
    this binary never sleeps, so an elapsed-time figure would describe the host
    rather than the replay, and would not be reproducible across processes.
    """
    lines = _run(repo_root, "--pacing", "fixed", "--interval-ns", "1000")
    metrics = lines[-1]
    assert metrics["type"] == "metrics"

    events = [line for line in lines if line["type"] == "event"]
    assert metrics["events_emitted"] == len(events)
    # One wait per event after the first, each the configured interval.
    assert metrics["total_scheduled_wait_nanos"] == 1000 * (len(events) - 1)
    # events per 10^9 ns of scheduled virtual time, as an integer.
    expected = (len(events) * 1_000_000_000) // metrics["total_scheduled_wait_nanos"]
    assert metrics["events_per_virtual_second_scaled"] == expected


def test_rate_metric_tracks_the_configured_rate(repo_root):
    """Halving the interval must double the achieved rate, or the metric is
    not measuring rate control at all."""
    slow = _run(repo_root, "--pacing", "fixed", "--interval-ns", "2000")[-1]
    fast = _run(repo_root, "--pacing", "fixed", "--interval-ns", "1000")[-1]
    # The metric is a scaled integer (a float would make byte-identical
    # cross-process comparison depend on formatting), so the doubling is exact
    # only up to one unit of floor-division truncation.
    assert abs(fast["events_per_virtual_second_scaled"] - 2 * slow["events_per_virtual_second_scaled"]) <= 1
    assert slow["total_scheduled_wait_nanos"] == 2 * fast["total_scheduled_wait_nanos"]


def test_declared_delay_is_realized_in_the_schedule(repo_root):
    """AEGIS-060: a delayed observation must actually be delayed.

    The delay is applied to the record's scheduled wait, not to its position:
    moving the record would perturb the canonical order (ADR-0019). So the
    faulted record keeps its `record_index` and its place, and carries a
    strictly larger scheduled wait than it would without the fault.
    """
    clean = {
        line["record_index"]: line
        for line in _run(repo_root, "--pacing", "step")
        if line["type"] == "event"
    }
    faulted = {
        line["record_index"]: line
        for line in _run(repo_root, "--pacing", "step", "--fault", "3:delayed:500")
        if line["type"] == "event"
    }

    assert faulted[3]["fault"] == "delayed"
    assert faulted[3]["fault_delay_nanos"] == 500
    assert faulted[3]["scheduled_wait_nanos"] == clean[3]["scheduled_wait_nanos"] + 500
    # Position and identity are untouched.
    assert list(faulted) == list(clean)
    assert faulted[3]["event_time_ns"] == clean[3]["event_time_ns"]
    # Only the targeted record is delayed.
    assert all(faulted[i]["fault_delay_nanos"] == 0 for i in faulted if i != 3)


def test_latency_spike_is_also_realized_in_the_schedule(repo_root):
    """kLatencySpike carries its magnitude in `delay` (ADR-0019 slice 12)."""
    faulted = [
        line
        for line in _run(repo_root, "--pacing", "step", "--fault", "2:latency_spike:750")
        if line["type"] == "event" and line["record_index"] == 2
    ]
    assert faulted[0]["scheduled_wait_nanos"] == 750


def test_interactive_mode_advances_one_timestamp_group_per_command(repo_root):
    """AEGIS-057's interactive half: advancement is externally driven.

    Two lines of input must yield two timestamp groups, not the whole stream --
    the property that distinguishes a stepped consumer from a drain.
    """
    binary = resolve_replay_run_binary(repo_root)
    result = subprocess.run(
        [
            str(binary),
            "--input", str(repo_root / FUTURES_REPLAY_STREAM_RELATIVE_PATH),
            "--label", FUTURES_REPLAY_STREAM_LABEL,
            "--pacing", "step",
            "--interactive",
        ],
        input="\n\n",
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    lines = [json.loads(line) for line in result.stdout.splitlines()]
    stepped = _record_indices(lines)

    full = _record_indices(_run(repo_root, "--pacing", "step"))
    assert 0 < len(stepped) < len(full), "interactive mode must stop, not drain"
    assert stepped == full[: len(stepped)]


def test_interactive_mode_stops_on_quit(repo_root):
    binary = resolve_replay_run_binary(repo_root)
    result = subprocess.run(
        [
            str(binary),
            "--input", str(repo_root / FUTURES_REPLAY_STREAM_RELATIVE_PATH),
            "--label", FUTURES_REPLAY_STREAM_LABEL,
            "--pacing", "step",
            "--interactive",
        ],
        input="\nquit\n\n",
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    lines = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(_record_indices(lines)) == 1


def test_missing_binary_raises_not_skips(repo_root, monkeypatch, tmp_path):
    """A missing binary must fail the check, never silently skip it."""
    monkeypatch.setenv("AEGIS_REPLAY_RUN", str(tmp_path / "does-not-exist"))
    with pytest.raises(FileNotFoundError, match="aegis_replay_run not found"):
        resolve_replay_run_binary(repo_root)


def test_evidence_claim_is_replay_specific_not_the_m0_platform_wording(repo_root, tmp_path):
    """The M0 'not a claim that AEGIS is deterministic' wording must not travel
    onto replay evidence, which is exactly that claim for the replay core."""
    evidence_dir = tmp_path / "evidence"
    assert (
        determinism_check.main(
            [
                "--root", str(repo_root),
                "--producer", "futures_replay",
                "--runs", "2",
                "--write-evidence", str(evidence_dir),
            ]
        )
        == 0
    )
    summary = json.loads((evidence_dir / "summary.json").read_text(encoding="utf-8"))
    assert "not a claim that AEGIS is deterministic" not in summary["claim"]
    assert "AEGIS-058" in summary["claim"]
    assert summary["stable"] is True


def test_producer_ignores_seed(repo_root):
    """Replay reads no random source; fault rules are committed data."""
    assert run_producer("futures_replay", 1, repo_root) == run_producer("futures_replay", 99, repo_root)
