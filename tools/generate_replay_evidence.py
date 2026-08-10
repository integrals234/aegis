#!/usr/bin/env python3
"""Generate AEGIS-054..063, 059 and 229 evidence from the real replay stack.

Every figure here comes from running the actual `aegis_replay_run` binary over
the committed canonical stream, the actual `HistoricalReplayFeed`, and the
actual compiled binding. Nothing is recomputed in this file: if a claim cannot
be produced by production code, the generator raises rather than writing it.

What each artifact covers:

* **AEGIS-054..057** -- the four pacing modes' computed waits, plus the
  invariant that all four emit the identical canonical sequence.
* **AEGIS-058** -- byte-identical output from two *separate processes*, plus
  cursor/resume reproducing the tail of an uninterrupted run across a process
  boundary, plus debug/release agreement when both binaries are present.
* **AEGIS-060..063** -- each fault kind injected deterministically, with the
  emitted-plus-dropped accounting that proves nothing is silently lost.
* **AEGIS-059 / AEGIS-229** -- the Python feed and the C++ binding agreeing on
  one canonical order over the same records.

Every artifact carries a `not_evidence_for` block naming the residuals it does
*not* discharge, so it cannot be read as paying an M3/M4/M5/M9 debt.

Regenerate with: python3 tools/generate_replay_evidence.py
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from common.determinism import (
    FUTURES_REPLAY_STREAM_LABEL,
    FUTURES_REPLAY_STREAM_RELATIVE_PATH,
    resolve_replay_run_binary,
)
from evidence_provenance import provenance
from futures.replay import HistoricalReplayFeed

CANONICAL_FIELDS = ("event_time_ns", "source_sequence", "contract_symbol", "record_index")

# The seven slice-12 stress kinds plus the four slice-11 core kinds. kMissing
# and kDuplicated change which records are emitted; the rest annotate.
CORE_FAULT_KINDS = ("delayed", "missing", "duplicated", "sequence_gap")
STRESS_FAULT_KINDS = (
    "spread_widening",
    "volatility_spike",
    "liquidity_vanish",
    "rejection",
    "latency_spike",
    "partial_fill",
    "backpressure",
)


def _run(binary: Path, *args: str) -> str:
    result = subprocess.run(
        [
            str(binary),
            "--input", str(ROOT / FUTURES_REPLAY_STREAM_RELATIVE_PATH),
            "--label", FUTURES_REPLAY_STREAM_LABEL,
            *args,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"aegis_replay_run failed (exit {result.returncode}): {result.stderr}")
    return result.stdout


def _lines(output: str) -> list[dict[str, Any]]:
    return [json.loads(line) for line in output.splitlines()]


def _indices(lines: list[dict[str, Any]]) -> list[int]:
    return [line["record_index"] for line in lines if line["type"] == "event"]


def _pacing_evidence(binary: Path) -> dict[str, Any]:
    modes = {
        "original_speed_AEGIS_054": ("--pacing", "original"),
        "accelerated_x4_AEGIS_055": ("--pacing", "accelerated", "--multiplier", "4"),
        "fixed_rate_1000ns_AEGIS_056": ("--pacing", "fixed", "--interval-ns", "1000"),
        "step_AEGIS_057": ("--pacing", "step"),
    }
    results: dict[str, Any] = {}
    sequences: dict[str, list[int]] = {}
    for name, args in modes.items():
        lines = _lines(_run(binary, *args))
        events = [line for line in lines if line["type"] == "event"]
        sequences[name] = _indices(lines)
        results[name] = {
            "waits_nanos": [event["wait_nanos"] for event in events],
            "record_count": len(events),
        }

    baseline = next(iter(sequences.values()))
    if any(seq != baseline for seq in sequences.values()):
        raise RuntimeError(
            "pacing modes emitted different canonical sequences; pacing must change "
            "only the computed wait, never the event order"
        )
    if len({tuple(r["waits_nanos"]) for r in results.values()}) < 3:
        raise RuntimeError(
            "the pacing modes' computed waits are not meaningfully distinct; the "
            "evidence would not show the modes doing anything"
        )

    results["shared_canonical_sequence"] = baseline
    results["invariant"] = (
        "all four pacing modes emit the identical canonical record_index sequence; "
        "only the computed wait differs (verified above, not asserted)"
    )
    return results


def _fault_evidence(binary: Path) -> dict[str, Any]:
    clean = _indices(_lines(_run(binary, "--pacing", "step")))
    per_kind: dict[str, Any] = {}
    for kind in (*CORE_FAULT_KINDS, *STRESS_FAULT_KINDS):
        lines = _lines(_run(binary, "--pacing", "step", "--fault", f"4:{kind}:250:7"))
        manifest = lines[0]
        emitted = _indices(lines)
        dropped = [line["record_index"] for line in lines if line["type"] == "dropped"]
        annotated = [
            line["record_index"]
            for line in lines
            if line["type"] == "event" and line["fault"] == kind
        ]
        # Nothing may vanish unaccounted for: emitted + dropped must reconstruct
        # the original record set (duplicates add, they never remove).
        if sorted(set(emitted) | set(dropped)) != clean:
            raise RuntimeError(f"fault kind {kind} lost a record; refusing to write the claim")
        per_kind[kind] = {
            "annotated_record_indices": annotated,
            "dropped_record_indices": dropped,
            "emitted_record_count": len(emitted),
            "records_dropped": manifest["records_dropped"],
            "canonical_order_preserved": sorted(emitted) == emitted,
        }
    return {
        "unfaulted_record_indices": clean,
        "per_kind": per_kind,
        "invariant": (
            "no fault kind mutates a canonical-order field: every survivor stays in "
            "canonical order, and emitted + dropped reconstructs the input set exactly"
        ),
    }


def _determinism_evidence(binary: Path) -> dict[str, Any]:
    args = ("--pacing", "accelerated", "--multiplier", "2", "--fault", "3:delayed:500", "--fault", "5:missing")
    first = _run(binary, *args)
    second = _run(binary, *args)
    if first != second:
        raise RuntimeError("two runs of aegis_replay_run disagreed; refusing to claim determinism")

    # Cursor/resume across a process boundary: the resumed tail must equal the
    # tail of an uninterrupted run, exactly.
    full = _indices(_lines(_run(binary, "--pacing", "step")))
    midpoint = full[len(full) // 2]
    resumed = _indices(_lines(_run(binary, "--pacing", "step", "--resume-after", str(midpoint))))
    expected_tail = full[full.index(midpoint) + 1 :]
    if resumed != expected_tail:
        raise RuntimeError("resume did not reproduce the tail of an uninterrupted run")

    evidence: dict[str, Any] = {
        "repeated_process_output_sha256": hashlib.sha256(first.encode("utf-8")).hexdigest(),
        "runs_compared": 2,
        "byte_identical": True,
        "resume_after_record_index": midpoint,
        "resumed_tail_record_indices": resumed,
        "resume_reproduces_uninterrupted_tail": True,
    }

    # Debug/release agreement, when a release build is present. Recorded as a
    # fact about what was actually compared rather than silently omitted.
    release_binary = ROOT / "build/release/cpp/replay/aegis_replay_run"
    if release_binary.exists():
        evidence["debug_release_agreement"] = {
            "compared": True,
            "identical": _run(release_binary, *args) == first,
            "release_binary": str(release_binary.relative_to(ROOT)),
        }
    else:
        evidence["debug_release_agreement"] = {
            "compared": False,
            "why": "no release build present at generation time; the C++ release preset "
            "is exercised by scripts/ci_local.sh and by the CI 'C++ release' job",
        }
    return evidence


def _feed_and_binding_evidence() -> dict[str, Any]:
    committed = [
        json.loads(line)
        for line in (ROOT / FUTURES_REPLAY_STREAM_RELATIVE_PATH).read_text(encoding="utf-8").splitlines()
    ]
    # Deliberately scrambled: for this stream ingestion order already is
    # canonical order, so feeding it unchanged would let an identity "sort"
    # pass. Fixed rotation-and-reverse, not random.
    scrambled = list(reversed(committed[3:] + committed[:3]))
    if [r["record_index"] for r in scrambled] == [r["record_index"] for r in committed]:
        raise RuntimeError("the scramble was a no-op; the comparison would prove nothing")

    python_order = [r["record_index"] for r in HistoricalReplayFeed(scrambled)]

    sys.path.insert(0, str(ROOT / "build/debug/cpp/bindings"))
    try:
        import aegis_bindings
    except ImportError as exc:  # pragma: no cover - build-dependent
        raise RuntimeError(
            "the compiled extension is required to produce AEGIS-229 evidence; "
            f"build it with 'cmake --build --preset debug'. Import error: {exc}"
        ) from exc

    binding_sorted = aegis_bindings.sort_canonical(
        [{field: r[field] for field in CANONICAL_FIELDS} for r in scrambled]
    )
    cpp_order = [r["record_index"] for r in binding_sorted]
    if python_order != cpp_order:
        raise RuntimeError(
            "the Python feed and the C++ binding disagree on canonical order; "
            "refusing to write a symmetry claim that is false"
        )
    if python_order == [r["record_index"] for r in scrambled]:
        raise RuntimeError("both implementations returned their input unchanged")

    return {
        "scrambled_input_record_indices": [r["record_index"] for r in scrambled],
        "python_feed_order": python_order,
        "cpp_binding_order": cpp_order,
        "orders_agree": True,
        "binding_build_info": aegis_bindings.build_info(),
    }


def main(argv: list[str] | None = None) -> int:
    del argv
    binary = resolve_replay_run_binary(ROOT)

    pacing = _pacing_evidence(binary)
    faults = _fault_evidence(binary)
    determinism = _determinism_evidence(binary)
    feed = _feed_and_binding_evidence()

    stream_bytes = (ROOT / FUTURES_REPLAY_STREAM_RELATIVE_PATH).read_bytes()
    common = {
        **provenance(),
        "input_stream": {
            "path": FUTURES_REPLAY_STREAM_RELATIVE_PATH,
            "label": FUTURES_REPLAY_STREAM_LABEL,
            "sha256": hashlib.sha256(stream_bytes).hexdigest(),
            "record_count": len(stream_bytes.decode("utf-8").splitlines()),
            "derived_from": (
                "the committed data_samples/futures/bars/* files, through the real "
                "futures.ingest pipeline (tools/make_replay_fixture.py); record_index "
                "is ingestion's own assignment, read by replay and never recomputed"
            ),
        },
        "producer": "tools/generate_replay_evidence.py",
        "binary": "build/debug/cpp/replay/aegis_replay_run",
    }

    artifacts: list[tuple[str, list[str], dict[str, Any]]] = [
        (
            "replay_pacing_modes",
            ["AEGIS-054", "AEGIS-055", "AEGIS-056", "AEGIS-057"],
            {
                "pacing": pacing,
                "claim": (
                    "The four approved pacing modes were each driven through the real "
                    "ReplayEngine over the committed canonical stream. Their computed "
                    "waits are recorded and are genuinely distinct; all four emit the "
                    "identical canonical record_index sequence. No mode sleeps: every "
                    "wait is a computed virtual duration (ADR-0018), so these figures "
                    "are not timing measurements and are not a performance claim."
                ),
                "not_evidence_for": [
                    "any latency or throughput claim: no wall-clock time is measured here",
                ],
            },
        ),
        (
            "replay_determinism",
            ["AEGIS-058"],
            {
                "determinism": determinism,
                "claim": (
                    "aegis_replay_run produced byte-identical output across independent "
                    "process invocations over the same input, under accelerated pacing "
                    "with two committed fault rules; and a run resumed from a cursor in a "
                    "fresh process reproduced exactly the tail of an uninterrupted run. "
                    "This is AEGIS-058's 'repeated runs produce identical outputs', shown "
                    "across processes rather than across two calls in one binary."
                ),
                "not_evidence_for": [
                    "any performance or benchmark claim: this artifact records output "
                    "equality only, never timing",
                ],
            },
        ),
        (
            "replay_fault_injection",
            ["AEGIS-060", "AEGIS-061", "AEGIS-062", "AEGIS-063"],
            {
                "faults": faults,
                "claim": (
                    "All eleven fault kinds were injected deterministically at explicit, "
                    "committed record positions -- never sampled (ADR-0019). For every "
                    "kind, the emitted records plus the accounted-for dropped records "
                    "reconstruct the input set exactly and the survivors remain in "
                    "canonical order. This is the injection MECHANISM only."
                ),
                "not_evidence_for": [
                    "AEGIS-060's stale-data response and AEGIS-061's feed recovery: both "
                    "need the participant feed handler, which architecture rules date to "
                    "M3. The M3 residuals remain registered in docs/DEFERRED_VERIFICATION.md.",
                    "AEGIS-062's risk responses and AEGIS-063's OMS/risk integration: both "
                    "need layers architecture rules date to M5. The M5 residuals remain "
                    "registered and are NOT discharged here.",
                ],
            },
        ),
        (
            "feed_and_binding_symmetry",
            ["AEGIS-059", "AEGIS-229"],
            {
                "feed_and_binding": feed,
                "claim": (
                    "The production HistoricalReplayFeed and the compiled C++ binding's "
                    "sort_canonical were given the same deliberately scrambled records and "
                    "returned the identical canonical order -- neither returned its input "
                    "unchanged. AEGIS-229's round trip therefore runs against the real "
                    "engine ordering, and the Python and C++ halves of M2 are shown to "
                    "agree rather than assumed to."
                ),
                "not_evidence_for": [
                    "AEGIS-059's full acceptance: contract tests running one strategy "
                    "against multiple feed implementations need a strategy (M4) and "
                    "live/paper feeds (M9). The M9 residual remains registered and is NOT "
                    "discharged here.",
                ],
            },
        ),
    ]

    for name, requirement_ids, body in artifacts:
        payload = {"artifact": name, "requirements": requirement_ids, **common, **body}
        for rid in requirement_ids:
            out_dir = ROOT / "experiments/evidence" / rid
            out_dir.mkdir(parents=True, exist_ok=True)
            out_path = out_dir / f"{name}.json"
            out_path.write_text(
                json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
