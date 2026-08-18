#!/usr/bin/env python3
"""Generate M5 risk evidence (AEGIS-120..138) and the M5 halves of the two
inherited fault-injection obligations (AEGIS-062, AEGIS-063).

Every artifact records the REAL GoogleTest acceptance cases executed against
the compiled production classes (``tools/evidence_gtest.run_suite``), never
a Python reimplementation of the same arithmetic -- the same discipline the
M3 generators established. AEGIS-120 additionally records that the
architecture checker genuinely FAILS on its committed bypass fixture, and
the two vertical-demo requirements record the real
``aegis_participant_run`` binary's own output.

Regenerate with: python3 tools/generate_risk_evidence.py
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

RISK_ENGINE_IMPL = [
    "cpp/participant/risk/risk_engine.cpp",
    "cpp/participant/risk/risk_engine.hpp",
    "cpp/participant/risk/risk_state.hpp",
    "cpp/participant/risk/risk_limits.hpp",
    "cpp/participant/risk/reason_codes.cpp",
]
SEAM_IMPL = [
    "cpp/participant/app/risk_engine_gate.hpp",
    "cpp/participant/app/risk_engine_gate.cpp",
]

SYNTHETIC_DATA_NOTE = (
    "All market data driving these tests is synthetic and in-repo: venue SYNX does not "
    "exist, quotes are constructed from committed daily bar/fixture data rather than "
    "observed tick data (ADR-0025), and no conclusion about any real market may be drawn."
)
SIMULATION_NOTE = (
    "These are software risk controls exercised under simulation. Passing here is not "
    "evidence of production risk adequacy, live execution realism, or profitability."
)
MARGIN_NOTE = (
    "The margin model is the documented simplified Model A "
    "(margin_per_contract_units * abs(quantity), currency per contract -- no contract "
    "multiplier is applied a second time). It is NOT SPAN, NOT an exchange clearing "
    "model, and NOT a claim of production margin adequacy (ADR-0028)."
)

# requirement -> (artifact, title, frozen acceptance, gtest filters, extra implementation)
SPECS: dict[str, dict[str, Any]] = {
    "AEGIS-121": {
        "artifact": "max_order_quantity",
        "title": "Maximum order quantity",
        "acceptance": "Boundary tests pass.",
        "filters": ["RiskEngineOrderQuantity.*"],
        "claim": "Per-instrument quantity caps enforced at, below and above the limit, in both "
                 "configured modes (reject, and resize-to-cap).",
    },
    "AEGIS-122": {
        "artifact": "max_position_and_reservations",
        "title": "Maximum position",
        "acceptance": "Boundary and concurrent-fill tests pass.",
        "filters": [
            "RiskEnginePositionLimits.*",
            "ExecutionStressIntegration.RejectionReleasesTheReservation",
            "ExecutionStressIntegration.PartialFillReducesReservationWithoutDoubleCounting",
            "ExecutionStressIntegration.BackpressureAutomaticallyReleasesTheReservationThroughTheNormalLifecycle",
            "ExecutionStressIntegration.ReleaseReservationRemainsAvailableForOtherRecoveryPaths",
        ],
        "claim": "Long and short caps enforced against the PROJECTED position (confirmed position "
                 "+ every outstanding reservation + the candidate), so two orders that each pass "
                 "alone but jointly breach are rejected. Reservations are released automatically "
                 "on fill, partial fill, cancel, downstream rejection and failed/backpressured "
                 "submission -- the last via app::RiskReleasingExecutionAdapter, with no manual "
                 "caller cleanup required.",
        "implementation": SEAM_IMPL,
    },
    "AEGIS-123": {
        "artifact": "max_notional_and_currency",
        "title": "Maximum notional",
        "acceptance": "Multi-currency normalization tests pass or unsupported scope is explicit.",
        "filters": ["RiskEngineNotional.*"],
        "claim": "Per-order and portfolio notional caps enforced. An instrument whose currency has "
                 "no configured FX rate is rejected explicitly (kUnsupportedCurrency), never "
                 "silently treated as 1:1 with base; a configured non-base rate normalizes "
                 "correctly. Every in-repo product is USD, so the multi-currency path is exercised "
                 "only against a synthetic fixture -- stated, not implied.",
    },
    "AEGIS-124": {
        "artifact": "market_and_sector_exposure",
        "title": "Per-market and sector exposure",
        "acceptance": "Portfolio fixtures pass.",
        "filters": ["RiskEngineExposure.*"],
        "claim": "Grouped market/sector exposure limits enforced over the combined position + "
                 "reservation view, using instrument metadata from configuration.",
    },
    "AEGIS-125": {
        "artifact": "price_collars",
        "title": "Price collars",
        "acceptance": "Stale/reference edge cases pass.",
        "filters": ["RiskEnginePriceCollar.*", "MarketStressRisk.SpreadWideningTripsThePriceCollar"],
        "claim": "Configurable basis-point collars enforced against the last VALID reference "
                 "price. A stale or structurally invalid update never becomes the new reference, "
                 "and an instrument with no reference yet is rejected (kNoReferencePrice) rather "
                 "than defaulting to permissive.",
    },
    "AEGIS-126": {
        "artifact": "stale_data_rejection",
        "title": "Stale-data rejection",
        "acceptance": "Fault-injection test passes.",
        "filters": ["RiskEngineStaleData.*", "MarketStressRisk.LiquidityVanishTripsStaleness"],
        "claim": "Trading is blocked on stale market state (quote age beyond max_quote_age on the "
                 "injected virtual clock) and on structurally invalid state. Driven by M2's own "
                 "deterministic fault machinery via run_market_stress_risk_scenario, not a "
                 "hand-set flag.",
    },
    "AEGIS-127": {
        "artifact": "duplicate_order_protection",
        "title": "Duplicate-order protection",
        "acceptance": "Idempotency tests pass.",
        "filters": ["RiskEngineIdempotency.*"],
        "claim": "A replayed proposal leg (same strategy_id|proposal_id|leg_index) is rejected "
                 "kDuplicateRequest within the engine's lifetime. No cross-process persistence is "
                 "claimed; the frozen acceptance does not require it (ADR-0028).",
    },
    "AEGIS-128": {
        "artifact": "message_rate_limits",
        "title": "Message-rate limits",
        "acceptance": "Virtual-clock tests pass.",
        "filters": ["RiskEngineRateLimit.*"],
        "claim": "Order AND cancel rates are independently throttled on the injected virtual "
                 "clock, with window decay verified. Safety-driven cancels (kill switch, "
                 "connectivity loss) bypass the client throttle by construction -- a message-rate "
                 "budget must never be able to block the system's own ability to flatten.",
    },
    "AEGIS-129": {
        "artifact": "margin_availability",
        "title": "Margin availability",
        "acceptance": "Scenario fixtures pass and limitations are stated.",
        "filters": ["RiskEngineMarginAndLeverage.RejectsInsufficientMargin"],
        "claim": "Available margin enforced against equity using the documented Model A. Boundary "
                 "verified at exactly-equal and one-contract-over.",
        "extra_limitations": [MARGIN_NOTE],
    },
    "AEGIS-130": {
        "artifact": "max_leverage",
        "title": "Maximum leverage",
        "acceptance": "Boundary tests pass.",
        "filters": ["RiskEngineMarginAndLeverage.RejectsExcessiveLeverage"],
        "claim": "gross notional / equity enforced with boundary cases at, below and above the "
                 "limit. Non-positive equity supports no leverage at all -- rejected explicitly "
                 "rather than dividing by a non-positive number.",
    },
    "AEGIS-131": {
        "artifact": "daily_loss_limit",
        "title": "Daily loss limit",
        "acceptance": "Loss-path scenario triggers exactly once.",
        "filters": ["RiskEngineDailyLoss.*"],
        "claim": "The latch trips exactly once: the first breach transitions state, and deeper "
                 "losses while already tripped emit no second trip. Subsequent proposals are "
                 "rejected kTradingHalted. A new session clears this latch only.",
    },
    "AEGIS-132": {
        "artifact": "max_drawdown",
        "title": "Maximum drawdown",
        "acceptance": "High-water-mark scenarios pass.",
        "filters": ["RiskEngineDrawdown.*"],
        "claim": "High-water-mark drawdown (stats::DrawdownTracker) latches on breach. Recovery "
                 "past the old peak does NOT un-trip it, and start_new_session deliberately does "
                 "not clear it -- a max-ever quantity has no session boundary.",
    },
    "AEGIS-133": {
        "artifact": "volatility_triggered_reduction",
        "title": "Volatility-triggered reduction",
        "acceptance": "Deterministic sizing fixtures pass.",
        "filters": [
            "RiskEngineVolatilityReduction.*",
            "MarketStressRisk.VolatilitySpikeResizesOrRejects",
        ],
        "claim": "Approved quantity scales down deterministically as realized volatility "
                 "(stats::RollingRealizedVolatility) rises above target, floored at 1 unit, and "
                 "rejects outright at or beyond hard_reject_multiple rather than resizing to a "
                 "token size.",
    },
    "AEGIS-134": {
        "artifact": "concentration_and_correlation",
        "title": "Concentration and correlation limits",
        "acceptance": "Portfolio scenarios pass.",
        "filters": ["RiskEngineConcentration.*"],
        "claim": "Single-instrument concentration share and configuration-supplied correlated-group "
                 "exposure limits enforced. Correlation is never estimated inside the decision "
                 "path (ADR-0028): a risk decision must not depend on a statistic derived from "
                 "the same stream the decision is about.",
    },
    "AEGIS-135": {
        "artifact": "kill_switches",
        "title": "Strategy and portfolio kill switches",
        "acceptance": "Kill-switch tests cancel/disable safely and prevent new orders.",
        "filters": [
            "RiskEngineKillSwitch.*",
            "CalendarSpreadRiskExchangeIntegration.GlobalKillSwitchCancelsLiveOrdersAndBlocksNewSubmissions",
        ],
        "claim": "Strategy-level and global kill switches are idempotent (a second trip is a "
                 "no-op returning false), strategy-scoped tripping leaves other strategies "
                 "unaffected, live orders receive safety cancels that bypass the ordinary cancel "
                 "throttle, and every subsequent proposal is rejected. Proven end to end against "
                 "a real unmodified M1 ExchangeNode: the exchange order count does not move after "
                 "the trip.",
        "implementation": SEAM_IMPL,
    },
    "AEGIS-136": {
        "artifact": "connectivity_loss_response",
        "title": "Connectivity-loss response",
        "acceptance": "Fault scenarios pass.",
        "filters": ["RiskEngineConnectivity.*"],
        "claim": "Feed, exchange and broker disconnects are three independent signals, each "
                 "blocking new orders with its own distinct reason code until reconnected -- a "
                 "feed outage does not imply an exchange outage.",
    },
    "AEGIS-137": {
        "artifact": "risk_decision_audit",
        "title": "Risk decision audit",
        "acceptance": "Every proposal has exactly one auditable risk decision.",
        "filters": ["RiskEngineAuditInvariant.*"],
        "claim": "Exactly ONE terminal ProposalRiskDecision per proposal_id (asserted via "
                 "RiskAuditLog::proposal_decision_count), with subordinate per-order "
                 "OrderRiskDecision records that each carry the proposal_id/leg_index that "
                 "authorised them. A two-leg spread therefore never produces two objects both "
                 "claiming to be the proposal decision. A rejected proposal arms no pending leg, "
                 "and an order reaching the seam anyway is rejected kUnexpectedOrder.",
    },
    "AEGIS-138": {
        "artifact": "portfolio_risk_analytics",
        "title": "Portfolio risk analytics",
        "acceptance": "Dashboard/report fixture reconciles with portfolio state.",
        "filters": ["PortfolioRiskAnalytics.*"],
        "claim": "Gross/net exposure, margin used/available, market and sector exposure, "
                 "volatility and drawdown contribution, and deterministic scripted stress results "
                 "are computed from real Portfolio state. The Python report side "
                 "(python/reports/portfolio_risk_report.py) INDEPENDENTLY recomputes gross/net "
                 "exposure from the raw position/price accounting records and reconciles against "
                 "these figures rather than re-serializing them; its own tests prove it flags a "
                 "deliberate mismatch.",
        "implementation": [
            "cpp/participant/portfolio/portfolio_risk.cpp",
            "cpp/participant/portfolio/portfolio_risk.hpp",
            "python/reports/portfolio_risk_report.py",
        ],
        "extra_limitations": [
            "Stress scenarios are scripted uniform parallel price shocks, not a statistical "
            "simulation; volatility_multiple/liquidity_factor are reported as context, not "
            "separately modelled as execution-quality effects.",
            MARGIN_NOTE,
        ],
    },
}

OBLIGATION_SPECS: dict[str, dict[str, Any]] = {
    "AEGIS-062": {
        "artifact": "market_stress_risk_response",
        "title": "Fault injection: market stress",
        "acceptance": "Scenario results and risk responses are reproducible.",
        "filters": ["MarketStressRisk.*"],
        "claim": "The M5 residual is paid: every market-stress fault kind M2 delivers "
                 "(kSpreadWidening, kVolatilitySpike, kLiquidityVanish) now produces a REAL "
                 "risk::RiskEngine response through run_market_stress_risk_scenario -- a genuine "
                 "evaluate() call, not a report generated afterward -- and two independently "
                 "constructed engines fed the identical rule set produce identical verdicts and "
                 "reason codes. No fault kind was added and "
                 "replay::DeterministicFaultInjector is consumed unmodified.",
        "implementation": [
            "cpp/participant/app/fault_scenario.cpp",
            "cpp/participant/app/fault_scenario.hpp",
        ],
        "m2_half": "experiments/evidence/AEGIS-062/replay_fault_injection.json",
    },
    "AEGIS-063": {
        "artifact": "execution_stress_oms_risk_integration",
        "title": "Fault injection: execution stress",
        "acceptance": "OMS/risk integration tests cover each fault.",
        "filters": ["ExecutionStressIntegration.*"],
        "claim": "The M5 residual is paid: all four execution-stress fault kinds (kRejection, "
                 "kLatencySpike, kPartialFill, kBackpressure) are covered by integration tests "
                 "driving the real risk::RiskEngine through the real mandatory OMS seam "
                 "(RiskEngineGate -> OrderManager), asserting risk reservation/audit state stays "
                 "coherent with what actually happened at the OMS. Backpressure releases its "
                 "reservation automatically.",
        "implementation": SEAM_IMPL,
        "m2_half": "experiments/evidence/AEGIS-063/replay_fault_injection.json",
    },
}


def _write(requirement: str, artifact: str, payload: dict[str, Any]) -> None:
    out_dir = EVIDENCE_ROOT / requirement
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{artifact}.json"
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")


def _run_demo(binary: Path, risk_config: str) -> dict[str, Any]:
    stream = ROOT / "tests/unit/fixtures/participant/calendar_spread_stream.jsonl"
    result = subprocess.run(
        [str(binary), "--calendar-spread", "--stream", str(stream),
         "--risk-config", str(ROOT / risk_config)],
        capture_output=True, text=True, check=False,
    )
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    decisions = [json.loads(line)["risk_decision"] for line in lines if "risk_decision" in json.loads(line)]
    orders = [json.loads(line)["order_count"] for line in lines]
    return {
        "risk_config": risk_config,
        "exit_code": result.returncode,
        "output_line_count": len(lines),
        "risk_decisions": decisions,
        "final_order_count": orders[-1] if orders else 0,
    }


def _architecture_gate_rejects_bypass() -> dict[str, Any]:
    """AEGIS-120's negative gate: the architecture checker must FAIL on the
    committed bypass fixture. A passing checker there would prove nothing."""
    fixture = "tests/unit/fixtures/arch_violation"
    result = subprocess.run(
        [sys.executable, "tools/check_architecture.py", "--root", fixture,
         "--rules", f"{fixture}/configs/architecture_rules.yaml"],
        cwd=ROOT, capture_output=True, text=True, check=False,
    )
    # check_architecture.py reports violations on stderr and exits nonzero;
    # gating on the exit code (not merely on parsed text) is what makes this a
    # real negative gate rather than a string search that a formatting change
    # could silently turn green.
    violations = [line for line in result.stderr.splitlines() if line.startswith("ERROR:")]
    bypass = [v for v in violations if "cpp-participant-oms" in v or "gateway" in v]
    return {
        "fixture": fixture,
        "checker_exit_code": result.returncode,
        "checker_rejected_the_fixture": result.returncode != 0 and bool(violations),
        "violation_count": len(violations),
        "strategy_to_oms_bypass_violations": bypass,
    }


def main() -> int:
    all_passed = True
    binary = resolve_participant_run_binary(ROOT)

    # --- AEGIS-120: structural, proven by a failing negative gate ----------
    gate = _architecture_gate_rejects_bypass()
    positive = subprocess.run(
        [sys.executable, "tools/check_architecture.py"], cwd=ROOT,
        capture_output=True, text=True, check=False,
    )
    suite_120 = merge(
        run_suite("RiskEngineAuditInvariant.RejectedProposalReportsEveryLegRejectedAndArmsNoOrder", root=ROOT),
        run_suite("CalendarSpreadRiskExchangeIntegration.RejectedProposalNeverReachesTheExchangeOrThePortfolio",
                  root=ROOT),
    )
    all_passed = all_passed and suite_120["all_passed"] and gate["checker_rejected_the_fixture"]
    _write("AEGIS-120", "mandatory_risk_path", {
        **provenance(),
        "artifact": "AEGIS-120/mandatory_risk_path",
        "producer": "tools/generate_risk_evidence.py",
        "requirements": ["AEGIS-120"],
        "title": "Mandatory risk path",
        "frozen_acceptance": "Architecture test rejects bypass dependencies.",
        "implementation": ["configs/architecture_rules.yaml", "tools/check_architecture.py", *SEAM_IMPL],
        "inputs": [gate["fixture"], "configs/architecture_rules.yaml"],
        "negative_gate": gate,
        "positive_architecture_check_passed": positive.returncode == 0,
        "acceptance_tests": suite_120,
        "claim": (
            "The bypass is rejected structurally, in both directions. NEGATIVE: "
            "tools/check_architecture.py FAILS on the committed arch_violation fixture, whose "
            "cpp/participant/strategy/rogue.hpp holds a gateway header and reaches the exchange "
            "book directly -- exactly the strategy->OMS/exchange bypass this requirement forbids. "
            "POSITIVE: the same checker passes on the real tree, where cpp-participant-strategy's "
            "may_depend_on carries no OMS, exchange or gateway edge. At runtime the seam is also "
            "closed: an order reaching RiskEngine::decide_order with no armed proposal decision is "
            "rejected kUnexpectedOrder, and a rejected proposal never reaches the exchange at all."
        ),
        "limitations": [SYNTHETIC_DATA_NOTE, SIMULATION_NOTE],
        "not_evidence_for": [
            "any claim that no bypass could ever be written -- the gate rejects the dependency "
            "shapes it enumerates, and a future bypass of a genuinely new shape would need a new "
            "rule",
            "any claim about a production gateway or broker adapter -- none exists before M9",
        ],
    })

    # --- AEGIS-121..138 ---------------------------------------------------
    for requirement, spec in SPECS.items():
        suite = merge(*[run_suite(flt, root=ROOT) for flt in spec["filters"]])
        all_passed = all_passed and suite["all_passed"]
        limitations = [SYNTHETIC_DATA_NOTE, SIMULATION_NOTE, *spec.get("extra_limitations", [])]
        _write(requirement, spec["artifact"], {
            **provenance(),
            "artifact": f"{requirement}/{spec['artifact']}",
            "producer": "tools/generate_risk_evidence.py",
            "requirements": [requirement],
            "title": spec["title"],
            "frozen_acceptance": spec["acceptance"],
            "implementation": [*RISK_ENGINE_IMPL, *spec.get("implementation", [])],
            "inputs": ["configs/risk/limits.json"],
            "deterministic_config": {
                "clock": "injected (common::ManualClock / explicit nanos) -- never the system clock",
                "risk_config": "configs/risk/limits.json and per-test in-code RiskLimitsConfig",
            },
            "acceptance_tests": suite,
            "claim": spec["claim"],
            "limitations": limitations,
            "not_evidence_for": [
                "production risk adequacy -- thresholds here are small documented demo values",
                "any real-market behaviour -- every input is synthetic (ADR-0025)",
            ],
        })

    # --- AEGIS-062 / AEGIS-063 M5 halves ----------------------------------
    for requirement, spec in OBLIGATION_SPECS.items():
        suite = merge(*[run_suite(flt, root=ROOT) for flt in spec["filters"]])
        all_passed = all_passed and suite["all_passed"]
        _write(requirement, spec["artifact"], {
            **provenance(),
            "artifact": f"{requirement}/{spec['artifact']}",
            "producer": "tools/generate_risk_evidence.py",
            "requirements": [requirement],
            "title": spec["title"],
            "frozen_acceptance": spec["acceptance"],
            "implementation": [*RISK_ENGINE_IMPL, *spec.get("implementation", [])],
            "inputs": ["cpp/replay/fault_injection.hpp"],
            "m2_half_of_this_obligation": spec["m2_half"],
            "acceptance_tests": suite,
            "claim": spec["claim"],
            "limitations": [SYNTHETIC_DATA_NOTE, SIMULATION_NOTE],
            "not_evidence_for": [
                "any claim that these are the only faults that could occur -- they are the fault "
                "kinds M2 deterministically injects, not an exhaustive taxonomy",
            ],
        })

    # --- The M5 vertical demo, from the real production binary -------------
    allow = _run_demo(binary, "configs/risk/limits.json")
    reject = _run_demo(binary, "configs/risk/limits_reject_demo.json")
    repeat = _run_demo(binary, "configs/risk/limits.json")
    demo_suite = run_suite("CalendarSpreadRiskExchangeIntegration.*", root=ROOT)
    all_passed = all_passed and demo_suite["all_passed"]
    _write("AEGIS-120", "vertical_risk_enforced_demo", {
        **provenance(),
        "artifact": "AEGIS-120/vertical_risk_enforced_demo",
        "producer": "tools/generate_risk_evidence.py",
        "requirements": ["AEGIS-120", "AEGIS-121", "AEGIS-135", "AEGIS-137"],
        "title": "M5 vertical demo: strategy -> proposal -> real risk -> OMS -> execution",
        "implementation": ["cpp/participant/app/participant_run.cpp", *SEAM_IMPL, *RISK_ENGINE_IMPL],
        "inputs": [
            "tests/unit/fixtures/participant/calendar_spread_stream.jsonl",
            "configs/risk/limits.json",
            "configs/risk/limits_reject_demo.json",
        ],
        "allow_run": allow,
        "reject_run": reject,
        "repeat_run_matches_allow_run": repeat == allow,
        "real_matching_acceptance_tests": demo_suite,
        "claim": (
            "The production CLI (aegis_participant_run --calendar-spread --risk-config) routes "
            "every proposal through the real risk::RiskEngine. Under configs/risk/limits.json the "
            "proposals are approved and real orders and fills occur (final_order_count > 0); under "
            "configs/risk/limits_reject_demo.json -- identical except the per-instrument quantity "
            "cap -- the SAME stream produces kMaxOrderQuantity rejections and final_order_count "
            "== 0. Two runs of the allow configuration are identical. Against a real, unmodified "
            "M1 ExchangeNode with real FIFO matching (tests/, outside covered_roots), the four "
            "acceptance tests prove allow (real fill), reject (zero adapter submit, zero exchange "
            "order, unchanged portfolio), resize (the exchange sees exactly the approved "
            "quantity) and global kill switch (live order cancelled, next proposal blocked)."
        ),
        "limitations": [SYNTHETIC_DATA_NOTE, SIMULATION_NOTE],
        "not_evidence_for": [
            "execution realism -- the production path's fills are a deterministic immediate-fill "
            "model; only the tests/ harness exercises real FIFO matching",
            "profitability -- the demo stream is six constructed observations",
        ],
    })

    print(f"\nall acceptance suites passed: {all_passed}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
