#!/usr/bin/env python3
"""Generate M3 market-data, reconstruction and microstructure evidence
(AEGIS-064..075) and the M3 half of the two inherited feed obligations
(AEGIS-060, AEGIS-061).

Every record here is produced by executing the real compiled acceptance
tests against the real production classes (`cpp/participant/feed_handler`,
`cpp/participant/book_builder`, `cpp/exchange/market_data`,
`cpp/participant/app`) -- see `tools/evidence_gtest.py` for why the tests
themselves are the evidence for criteria phrased as "fixtures pass". Each
recorded case names the source file and line of the assertion, so the claim
is checkable rather than merely asserted.

The end-to-end block additionally records the concrete values the real
`aegis_participant_run` binary computes over its built-in deterministic
scenario, so the artifact carries actual reconstructed numbers (best bid/
ask, microprice, rolling mean) and not only pass/fail.

**AEGIS-060 and AEGIS-061 are the M3 halves only.** M2 delivered the
deterministic fault *injection* mechanism and its own artifact
(`experiments/evidence/AEGIS-060/replay_fault_injection.json`, which says
so plainly). What is proven here is the participant's *response*: a delayed
observation driving the reconstructed book stale, and a
missing/duplicated/gap fault driving buffer-rebase-replay recovery.

Regenerate with: python3 tools/generate_participant_evidence.py
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from common.determinism import resolve_participant_run_binary
from evidence_gtest import merge, run_suite
from evidence_provenance import provenance

EVIDENCE_ROOT = ROOT / "experiments/evidence"

# One entry per requirement: the frozen acceptance criterion, the production
# code that implements it, and the gtest filters whose cases assert it.
RECONSTRUCTION: dict[str, dict[str, Any]] = {
    "AEGIS-064": {
        "title": "Full-depth snapshots",
        "acceptance": "Snapshot fixtures reconstruct expected levels and quantities.",
        "implementation": [
            "cpp/exchange/market_data/market_data_publisher.cpp",
            "cpp/participant/book_builder/book_builder.cpp",
        ],
        "filters": [
            "BookBuilder.ApplySnapshotReconstructsLevelsAndQuantities",
            "BookBuilder.LevelsReturnsBestFirstUpToDepth",
            "BookBuilder.ApplySnapshotDiscardsPriorState",
            "ExchangeNode.MarketDataSnapshotReflectsAcceptedOrder",
        ],
    },
    "AEGIS-065": {
        "title": "Incremental updates",
        "acceptance": "Incremental sequence matches golden books.",
        "implementation": ["cpp/participant/book_builder/book_builder.cpp"],
        "filters": [
            "BookBuilder.OrderLifecycleFixturesReconstructOrderLevelState",
            "BookBuilder.AggregatedOnlyDeltasReconstructPriceLevels",
            "BookBuilder.ModifyingAnUnknownOrderIsIgnored",
            "MarketDataPublisher.*",
            "ExchangeNode.MarketDataDelta*",
        ],
    },
    "AEGIS-066": {
        "title": "Order-level reconstruction",
        "acceptance": "Order lifecycle fixtures pass.",
        "implementation": ["cpp/participant/book_builder/book_builder.cpp"],
        "filters": [
            "BookBuilder.OrderLifecycleFixturesReconstructOrderLevelState",
            "BookBuilder.ModifyingAnUnknownOrderIsIgnored",
        ],
    },
    "AEGIS-067": {
        "title": "Price-level reconstruction",
        "acceptance": "Aggregated update fixtures pass.",
        "implementation": ["cpp/participant/book_builder/book_builder.cpp"],
        "filters": ["BookBuilder.AggregatedOnlyDeltasReconstructPriceLevels"],
    },
    "AEGIS-068": {
        "title": "Sequence validation",
        "acceptance": "Seeded faults trigger exact diagnostics.",
        "implementation": [
            "cpp/participant/feed_handler/sequence_tracker.cpp",
            "cpp/participant/feed_handler/feed_handler.cpp",
        ],
        "filters": ["SequenceTracker.*", "FeedHandler.*"],
    },
    "AEGIS-069": {
        "title": "Stale-data detection",
        "acceptance": "Virtual-clock tests pass.",
        "implementation": ["cpp/participant/book_builder/book_builder.cpp"],
        "filters": [
            "BookBuilder.StaleAfterElapsedTimeExceedsTheConfiguredThreshold",
            "BookBuilder.StaleAfterEnoughConsecutiveSequenceFaults",
            "BookBuilder.StalenessIsDisabledUntilConfigured",
        ],
    },
    "AEGIS-070": {
        "title": "Snapshot recovery",
        "acceptance": "Recovery fixtures end in correct state.",
        "implementation": ["cpp/participant/book_builder/book_builder.cpp"],
        "filters": [
            "BookBuilder.RecoveryBuffersThenRebasesThenReplaysSurvivingDeltas",
            "FeedRecovery.*",
        ],
    },
    "AEGIS-071": {
        "title": "Top-of-book metrics",
        "acceptance": "Numeric fixtures pass.",
        "implementation": ["cpp/participant/book_builder/book_builder.cpp"],
        "filters": [
            "BookBuilder.TopOfBookReportsBestBidAskSpreadAndMid",
            "BookBuilder.TopOfBookLeavesSpreadAndMidUnsetWithOnlyOneSide",
        ],
    },
    "AEGIS-072": {
        "title": "Microprice",
        "acceptance": "Numeric fixtures pass and edge cases are defined.",
        "implementation": ["cpp/participant/book_builder/book_builder.cpp"],
        "filters": [
            "BookBuilder.MicropriceWeightsTowardTheOppositeSidesPrice",
            "BookBuilder.MicropriceIsUndefinedWithoutBothSides",
        ],
    },
    "AEGIS-073": {
        "title": "Depth and order-book imbalance",
        "acceptance": "Numeric fixtures across multiple levels pass.",
        "implementation": ["cpp/participant/book_builder/book_builder.cpp"],
        "filters": ["BookBuilder.DepthImbalance*"],
    },
    "AEGIS-074": {
        "title": "Trade/cancellation intensity",
        "acceptance": "Online/offline equivalence tests pass.",
        "implementation": [
            "cpp/statistics/rolling_rate.cpp",
            "cpp/participant/app/intensity_tracker.cpp",
        ],
        "filters": ["RollingRate.*", "IntensityTracker.*"],
    },
    "AEGIS-075": {
        "title": "Queue depletion and adverse selection",
        "acceptance": "Scenario fixtures produce expected metrics.",
        "implementation": ["cpp/participant/book_builder/book_builder.cpp"],
        "filters": ["BookBuilder.QueueDepletion*", "BookBuilder.AdverseSelection*"],
    },
}

STALE_RESPONSE = {
    "AEGIS-060": {
        "title": "Delayed observations and stale-data response (M3 half)",
        "acceptance": "Scenario fixtures verify delayed observations and stale-data responses.",
        "implementation": [
            "cpp/participant/app/fault_scenario.cpp",
            "cpp/participant/book_builder/book_builder.cpp",
        ],
        "filters": ["StaleDataResponse.*"],
    },
    "AEGIS-061": {
        "title": "Feed recovery for each fault kind (M3 half)",
        "acceptance": "Feed recovery tests cover each fault.",
        "implementation": [
            "cpp/participant/app/fault_scenario.cpp",
            "cpp/participant/book_builder/book_builder.cpp",
            "cpp/participant/feed_handler/sequence_tracker.cpp",
        ],
        "filters": ["FeedRecovery.*"],
    },
}


def _end_to_end_values() -> dict[str, Any]:
    """Concrete numbers the real production pipeline computes.

    Not a second implementation of the pipeline: this shells out to the
    committed `aegis_participant_run` binary and records what it printed,
    so the artifact carries reconstructed values a reader can reproduce with
    one command.
    """
    binary = resolve_participant_run_binary(ROOT)
    result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
    summary = json.loads(result.stdout)
    return {
        "command": f"{binary.relative_to(ROOT)}",
        "pipeline": (
            "market-data messages -> feed handler (decode + sequence validation) -> "
            "book builder (reconstruction) -> microstructure features -> generic "
            "statistics -> OMS lifecycle -> portfolio"
        ),
        "computed": summary,
    }


def _write(requirement: str, filename: str, payload: dict[str, Any]) -> Path:
    directory = EVIDENCE_ROOT / requirement
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / filename
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    return path


def main() -> int:
    stamp = provenance(ROOT)
    end_to_end = _end_to_end_values()
    all_passed = True
    written: list[str] = []

    for requirement, spec in RECONSTRUCTION.items():
        suite = merge(*(run_suite(flt, root=ROOT) for flt in spec["filters"]))
        all_passed = all_passed and suite["all_passed"]
        payload: dict[str, Any] = {
            "artifact": f"{requirement}/book_reconstruction.json",
            "producer": "tools/generate_participant_evidence.py",
            "requirements": [requirement],
            "title": spec["title"],
            "frozen_acceptance": spec["acceptance"],
            "implementation": spec["implementation"],
            "acceptance_tests": suite,
            "claim": (
                f"{requirement} ({spec['title']}): the frozen acceptance criterion "
                f"“{spec['acceptance']}” is satisfied by "
                f"{suite['test_count']} acceptance test(s) executed against the real "
                "production implementation listed under 'implementation'. Each case "
                "below records the source file and line of the assertion made."
            ),
            "not_evidence_for": [
                "Any performance, latency or throughput property: no timing is "
                "measured here and none is claimed (docs/BENCHMARK_POLICY.md).",
                "Behaviour on real venue market data: every fixture is synthetic and "
                "committed in-repository; M3 ships no live or paper feed.",
                "M5 risk responses to stale or recovered data -- M3 delivers the "
                "feed/book-level response only (ADR-0021).",
            ],
            "all_claims_hold": suite["all_passed"],
            **stamp,
        }
        if requirement in {"AEGIS-064", "AEGIS-065", "AEGIS-071", "AEGIS-072"}:
            payload["end_to_end_production_run"] = end_to_end
        written.append(str(_write(requirement, "book_reconstruction.json", payload).relative_to(ROOT)))

    for requirement, spec in STALE_RESPONSE.items():
        suite = merge(*(run_suite(flt, root=ROOT) for flt in spec["filters"]))
        all_passed = all_passed and suite["all_passed"]
        filename = (
            "stale_data_response.json" if requirement == "AEGIS-060" else "feed_recovery.json"
        )
        fault_kinds = (
            ["delayed"]
            if requirement == "AEGIS-060"
            else ["missing", "duplicated", "sequence_gap"]
        )
        payload = {
            "artifact": f"{requirement}/{filename}",
            "producer": "tools/generate_participant_evidence.py",
            "requirements": [requirement],
            "title": spec["title"],
            "frozen_acceptance": spec["acceptance"],
            "implementation": spec["implementation"],
            "m2_fault_kinds_consumed": fault_kinds,
            "acceptance_tests": suite,
            "claim": (
                f"{requirement}: M2's deterministic fault injector (ADR-0019, "
                "unmodified) drives the fault kinds listed in "
                "'m2_fault_kinds_consumed' through the real participant feed "
                "handler and book builder, and the participant's own response is "
                "asserted -- "
                + (
                    "the reconstructed book is marked stale once the configured "
                    "threshold elapses, and is not marked stale when delivery is "
                    "within it."
                    if requirement == "AEGIS-060"
                    else "each fault kind is detected and drives buffer -> re-base on "
                    "a fresh snapshot -> replay of the surviving buffered deltas, "
                    "ending in the correct book state; a duplicate is skipped rather "
                    "than reapplied."
                )
                + " This is the M3 half of the obligation; the M2 injection mechanism "
                "has its own artifact (experiments/evidence/"
                + requirement
                + "/replay_fault_injection.json)."
            ),
            "not_evidence_for": [
                "M5 risk action on stale or recovered data (AEGIS-062/AEGIS-063): M3 "
                "delivers the feed/book-level response only. No risk engine exists.",
                "The M2 injection mechanism itself, which is proven separately by "
                "tools/generate_replay_evidence.py and is not re-derived here.",
                "AEGIS-237 process-boundary participant-state recovery: a different "
                "mechanism (a byte-stable file snapshot) for a different failure "
                "mode (ADR-0024).",
                "Recovery against a real venue feed: every fault and every message "
                "here is synthetic and committed in-repository.",
            ],
            "all_claims_hold": suite["all_passed"],
            **stamp,
        }
        written.append(str(_write(requirement, filename, payload).relative_to(ROOT)))

    for path in written:
        print(f"wrote {path}")
    if not all_passed:
        print("ERROR: at least one acceptance test failed; artifacts record the failure",
              file=sys.stderr)
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
