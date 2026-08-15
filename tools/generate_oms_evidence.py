#!/usr/bin/env python3
"""Generate M3 OMS, execution, cost and accounting evidence (AEGIS-108..119).

Every record here is produced by executing the real compiled acceptance
tests against the real production classes in `cpp/participant/oms` and
`cpp/participant/portfolio` (see `tools/evidence_gtest.py`). Each recorded
case names the source file and line of the assertion.

Two distinctions this artifact set is careful to preserve, because
collapsing either would overstate what M3 proves:

**Real matching versus scripted responses.** AEGIS-109/110/111/114 are
proven through `TransportExecutionAdapter` driving a *real* `ExchangeNode`
with unmodified M1 FIFO matching, composed in
`tests/cpp/unit/test_participant_exchange_integration.cpp` (legal only
because `tests/` sits outside `covered_roots`). `RecordedResponseAdapter`
contributes race and rejection shapes that no live engine can be made to
emit on demand, and cross-adapter equivalence for AEGIS-119 -- it is never
the sole proof of an execution requirement, and it proves nothing about
matching behaviour.

**Seam versus transport.** AEGIS-119's "environment-independent" means the
OMS names no exchange type. M3 ships a transport *seam* and no transport:
no socket, no broker protocol, no credential, no session or reconnect
logic, no external reconciliation. Paper/live connectivity is AEGIS-221/222
at M9.

Regenerate with: python3 tools/generate_oms_evidence.py
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
RECOVERY_FIXTURE = "tests/unit/fixtures/participant/recovery_scenario.jsonl"

REAL_MATCHING_NOTE = (
    "Proven against a real ExchangeNode running unmodified M1 FIFO matching, "
    "driven through TransportExecutionAdapter over a test-only "
    "ExecutionTransport composed in tests/ (outside covered_roots). No "
    "production participant->exchange dependency exists."
)

SPECS: dict[str, dict[str, Any]] = {
    "AEGIS-108": {
        "title": "Order lifecycle state machine",
        "acceptance": "Invalid transitions are rejected; transition tests cover all states.",
        "implementation": [
            "cpp/participant/oms/order_state.cpp",
            "cpp/participant/oms/order_lifecycle.hpp",
            "cpp/participant/oms/order_manager.cpp",
        ],
        "filters": ["OrderLifecycle.*", "OrderManager.*"],
        "artifact": "oms_lifecycle.json",
        "note": (
            "The transition table is exercised exhaustively over every ordered "
            "pair of the ten frozen states (OrderLifecycle.EveryStatePairIsExercised), "
            "and the mandatory risk seam is structural: no path reaches Submitted "
            "without passing RiskPending."
        ),
    },
    "AEGIS-109": {
        "title": "Market-order execution",
        "acceptance": "Integration tests reconcile fills and positions.",
        "implementation": [
            "cpp/participant/oms/order_manager.cpp",
            "cpp/participant/portfolio/portfolio.cpp",
        ],
        "filters": ["ParticipantExchangeIntegration.MarketOrder*"],
        "artifact": "execution_scenarios.json",
        "note": REAL_MATCHING_NOTE,
    },
    "AEGIS-110": {
        "title": "Passive limit execution",
        "acceptance": "Scenario tests cover fill and nonfill outcomes.",
        "implementation": ["cpp/participant/oms/order_manager.cpp"],
        "filters": ["ParticipantExchangeIntegration.PassiveLimit*"],
        "artifact": "execution_scenarios.json",
        "note": (
            REAL_MATCHING_NOTE
            + " The nonfill outcome (a passive order resting away from liquidity) "
            "and the later fill from a genuine aggressor are both asserted in the "
            "same scenario."
        ),
    },
    "AEGIS-111": {
        "title": "Aggressive limit execution",
        "acceptance": "Scenario tests cover multiple levels and residuals.",
        "implementation": ["cpp/participant/oms/order_manager.cpp"],
        "filters": ["ParticipantExchangeIntegration.AggressiveLimit*"],
        "artifact": "execution_scenarios.json",
        "note": (
            REAL_MATCHING_NOTE
            + " The scenario sweeps two resting price levels and leaves a genuine "
            "residual resting on the book."
        ),
    },
    "AEGIS-112": {
        "title": "Cancel/amend lifecycle",
        "acceptance": "Race fixtures produce deterministic outcomes.",
        "implementation": [
            "cpp/participant/oms/order_manager.cpp",
            "cpp/participant/oms/order_state.cpp",
            "cpp/participant/oms/recorded_response_adapter.cpp",
        ],
        "filters": [
            "OrderManager.RejectedCancel*",
            "OrderManager.FillRacing*",
            "OrderManager.CancelIsRefused*",
            "OrderManager.CancellationTerminates*",
            "RecordedResponseAdapter.*",
        ],
        "artifact": "execution_scenarios.json",
        "note": (
            "All four cancel-race outcomes are covered deterministically: the cancel "
            "wins, a fill wins the race, the exchange rejects the cancel and the "
            "order reverts to whichever live state it actually held, and a cancel "
            "before acknowledgement is refused without ever reaching the adapter. "
            "RecordedResponseAdapter supplies orderings no live engine can be made "
            "to emit on demand; it proves nothing about matching."
        ),
    },
    "AEGIS-113": {
        "title": "Network and exchange latency",
        "acceptance": "Latency attribution reconciles total path.",
        "implementation": [
            "cpp/participant/oms/latency_model.cpp",
            "cpp/participant/oms/order_manager.cpp",
        ],
        "filters": [
            "LatencyModel.*",
            "OrderManager.SubmittedOrderCarriesAFiveStageLatencyAttribution",
            "OrderManager.WithoutALatencyModelNoAttributionIsFabricated",
        ],
        "artifact": "latency_attribution.json",
        "note": (
            "All FIVE stages the requirement names are modelled -- feed, decision, "
            "gateway, exchange and acknowledgement -- each carried through its own "
            "typed clock domain (EventTime -> ReceiveTime -> DecisionTime -> "
            "SubmitTime -> ExchangeTime -> AckTime, the domains ADR-0002 defined). "
            "The M3 closure audit found an earlier version modelling only three "
            "stages, with a reconciles() that summed consecutive differences and so "
            "could not fail; both are corrected. Reconciliation is now checked "
            "against an INDEPENDENTLY SUPPLIED observed acknowledgement stamp "
            "(residual_against), and a dedicated case asserts that removing one "
            "stage changes the total -- so 'all five stages are attributed' is a "
            "falsifiable claim rather than an identity. Durations are committed "
            "configuration: nothing is sampled, no wall clock is read, and these "
            "are modelled values, NOT measurements. The model is wired into "
            "production: OrderManager::submit_new_order attributes the path for the "
            "market event that motivated each order and stores it on the tracked "
            "order, and a manager configured with no latency model records no "
            "attribution at all rather than a zero-latency one that would read as a "
            "measurement -- both asserted by the OrderManager cases below."
        ),
    },
    "AEGIS-114": {
        "title": "Partial fills",
        "acceptance": "Position/cash reconciliation passes.",
        "implementation": [
            "cpp/participant/oms/order_manager.cpp",
            "cpp/participant/portfolio/portfolio.cpp",
        ],
        "filters": [
            "OrderManager.PartialFills*",
            "ParticipantExchangeIntegration.*",
        ],
        "artifact": "execution_scenarios.json",
        "note": (
            REAL_MATCHING_NOTE
            + " Cumulative filled and remaining quantities are tracked across "
            "successive partial fills and reconciled against the portfolio's own "
            "position."
        ),
    },
    "AEGIS-115": {
        "title": "Queue-position approximation",
        "acceptance": "Model limitations are explicit and synthetic validation exists.",
        "implementation": ["cpp/participant/oms/queue_position_estimator.cpp"],
        "filters": ["QueuePositionEstimator.*"],
        "artifact": "queue_approximation.json",
        "note": (
            "This is an APPROXIMATION and is labelled as one in its own header. It "
            "assumes a caller-supplied cancellation rate and treats volume ahead as "
            "uniformly consumable; it observes no per-order queue truth, because a "
            "participant consuming a public feed has none. Validation is entirely "
            "synthetic -- no order-level ground truth is consulted anywhere."
        ),
    },
    "AEGIS-116": {
        "title": "Fees and slippage",
        "acceptance": "Net P&L reconciliation passes.",
        "implementation": [
            "cpp/participant/oms/cost_model.hpp",
            "cpp/participant/portfolio/portfolio.cpp",
        ],
        "filters": [
            "FeeSchedule.*",
            "Slippage.*",
            "CostModelReconciliation.*",
            "OrderManager.FillsAccrueFees*",
            "OrderManager.AdverseFillPrice*",
            "OrderManager.MarketOrdersAccrueNoSlippage*",
        ],
        "artifact": "costs_and_pnl.json",
        "note": (
            "Fees and slippage are applied INSIDE OrderManager's fill path: every "
            "trade accrues fees at the committed rate and signed slippage against "
            "the order's own price, onto the tracked order. The M3 closure audit "
            "found the cost model built but never called from production; the "
            "OrderManager.FillsAccrueFees/AdverseFillPrice cases below assert the "
            "wiring, not just the arithmetic. A market order carries no price "
            "(ADR-0011) so it accrues no slippage, which is asserted rather than "
            "assumed. The reconciliation computes net P&L two independent ways from "
            "the same fill -- gross at a reference price minus slippage minus fees, "
            "versus the portfolio's own realized P&L and cash -- and requires them "
            "equal. Fee rates are committed configuration, not a claim about any "
            "venue's real fee schedule. The M3 closure re-audit found the earlier "
            "reconciliation vacuous on the fee leg (the fee cancelled on both sides) "
            "and cash never asserted; it now reconciles the fee by DIFFERENCE against "
            "a fee-free run of the same fills and asserts cash outright. The app "
            "layer was also carrying two independent fee ledgers -- a fixture fee "
            "reaching the portfolio while the OMS accrued its own at a default zero "
            "rate -- and now shares one FeeSchedule between them."
        ),
    },
    "AEGIS-117": {
        "title": "Missed trades",
        "acceptance": "Attribution includes missed-trade statistics.",
        "implementation": ["cpp/participant/oms/missed_trade_tracker.hpp"],
        "filters": [
            "MissedTradeTracker.*",
            "OrderManager.CancelledOrderRecordsItsUntradedRemainder*",
            "OrderManager.FullyFilledOrderRecordsNoMissedTrade",
            "OrderManager.ExchangeRejectedOrderRecordsItsWholeSize*",
            "OrderManager.MissedTradeOpportunityCost*",
        ],
        "artifact": "costs_and_pnl.json",
        "note": (
            "Missed trades are recorded BY OrderManager on both terminal paths: an "
            "exchange rejection records the whole requested size, and a termination "
            "records original minus cumulative filled (zero for a fully filled "
            "order, the residual for a cancel). The M3 closure audit found the "
            "tracker built but never called from production; the OrderManager.* "
            "cases below assert it is reached through those real paths. It "
            "deliberately does NOT model counterfactual profit -- that would need a "
            "strategy (M4) and is not claimed. Risk-gate rejections are excluded: a "
            "self-imposed decision is not a missed market opportunity. Opportunity cost "
            "(named in the frozen description) IS computed: each record keeps the "
            "missed order's own price and side, and total_opportunity_cost_units() "
            "measures the cost against a mark the CALLER supplies -- the OMS observes "
            "no market and will not invent one, so the mark is a parameter of the "
            "query rather than a fabricated field."
        ),
    },
    "AEGIS-118": {
        "title": "Position and cash accounting",
        "acceptance": "Double-entry-style reconciliation/property tests pass.",
        "implementation": ["cpp/participant/portfolio/portfolio.cpp"],
        "filters": ["Portfolio.*"],
        "property_filters": ["PortfolioConservation.*"],
        "artifact": "portfolio_accounting.json",
        "note": (
            "The double-entry identity cash + quantity*average_price - realized_pnl "
            "== -fees is checked after every fill over generated sequences, single- "
            "and multi-instrument. The generator never adds to an already-open "
            "position in the same direction, because Portfolio's average-price "
            "update uses integer division there and compounds truncation -- a real, "
            "documented property of the production code, stated rather than hidden."
        ),
    },
    "AEGIS-119": {
        "title": "Environment-independent OMS",
        "acceptance": "Contract tests pass across adapters.",
        "implementation": [
            "cpp/participant/oms/execution_adapter.hpp",
            "cpp/participant/oms/execution_transport.hpp",
            "cpp/participant/oms/transport_execution_adapter.cpp",
            "cpp/participant/oms/recorded_response_adapter.cpp",
        ],
        "filters": [
            "AdapterContract.*",
            "TransportExecutionAdapter.*",
            "RecordedResponseAdapter.*",
        ],
        "artifact": "adapter_contract.json",
        "note": (
            "The same OrderManager call sequence is run once through "
            "TransportExecutionAdapter (fronting a real ExchangeNode) and once "
            "through RecordedResponseAdapter (fronting a committed script), and the "
            "resulting lifecycle states are required identical for equivalently "
            "shaped responses. Both adapters are defined purely over cpp/events "
            "types and name no exchange type."
        ),
    },
}

COMMON_NOT_EVIDENCE_FOR = [
    "Any latency, throughput or comparative performance claim: AEGIS-113 models "
    "committed deterministic durations and measures nothing "
    "(docs/BENCHMARK_POLICY.md, docs/CV_CLAIMS_POLICY.md).",
    "Paper or live broker connectivity (AEGIS-221/AEGIS-222 at M9): M3 ships a "
    "transport seam and no transport -- no socket, no broker protocol, no "
    "credential, no session/reconnect logic, no external account reconciliation.",
    "Any risk policy, position limit, order limit or kill switch (AEGIS-120 and "
    "the M5 risk layer): M3 delivers the mandatory RiskGate seam only, and ships "
    "no production implementation of it.",
    "Any strategy or trading-performance result: M3 contains no strategy logic "
    "(M4) and no trading returns are computed or claimed anywhere.",
    "Behaviour against real venue data or a real counterparty: every scenario is "
    "synthetic and committed in-repository.",
]


def _fixture_final_state() -> dict[str, Any]:
    """The concrete OMS + portfolio state the real binary reaches.

    Recorded so the accounting artifacts carry actual numbers, not only
    pass/fail, and so a reader can reproduce them with one command.
    """
    binary = resolve_participant_run_binary(ROOT)
    result = subprocess.run(
        [str(binary), "--fixture", str(ROOT / RECOVERY_FIXTURE)],
        capture_output=True,
        text=True,
        check=True,
    )
    lines = result.stdout.splitlines()
    return {
        "command": f"{binary.relative_to(ROOT)} --fixture {RECOVERY_FIXTURE}",
        "step_count": len(lines),
        "final_state": json.loads(lines[-1]),
    }


def main() -> int:
    stamp = provenance(ROOT)
    fixture_state = _fixture_final_state()
    all_passed = True

    for requirement, spec in SPECS.items():
        suites = [run_suite(flt, root=ROOT) for flt in spec["filters"]]
        suites += [
            run_suite(flt, kind="property", root=ROOT)
            for flt in spec.get("property_filters", [])
        ]
        suite = merge(*suites)
        all_passed = all_passed and suite["all_passed"]

        payload: dict[str, Any] = {
            "artifact": f"{requirement}/{spec['artifact']}",
            "producer": "tools/generate_oms_evidence.py",
            "requirements": [requirement],
            "title": spec["title"],
            "frozen_acceptance": spec["acceptance"],
            "implementation": spec["implementation"],
            "acceptance_tests": suite,
            "interpretation": spec["note"],
            "claim": (
                f"{requirement} ({spec['title']}): the frozen acceptance criterion "
                f"“{spec['acceptance']}” is satisfied by {suite['test_count']} "
                "acceptance test(s) executed against the real production "
                "implementation listed under 'implementation'. See 'interpretation' "
                "for exactly what the tests do and do not establish."
            ),
            "not_evidence_for": COMMON_NOT_EVIDENCE_FOR,
            "all_claims_hold": suite["all_passed"],
            **stamp,
        }
        if requirement in {"AEGIS-114", "AEGIS-116", "AEGIS-118"}:
            payload["production_run_state"] = fixture_state

        directory = EVIDENCE_ROOT / requirement
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / spec["artifact"]
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        print(f"wrote {path.relative_to(ROOT)}")

    if not all_passed:
        print("ERROR: an OMS acceptance test failed", file=sys.stderr)
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
