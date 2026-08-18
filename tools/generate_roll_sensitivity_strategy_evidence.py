#!/usr/bin/env python3
"""Generate AEGIS-024 evidence: the M4 residual on roll-method strategy
sensitivity.

Runs the actual `research.roll_method_sensitivity.
compute_roll_method_strategy_sensitivity` -- itself a thin composition of M2's
unmodified `futures.roll_sensitivity.compare_roll_methods` and the real
`research.strategy_replay.replay_strategy` -- over two roll policies that
genuinely choose different roll dates.

**A finding worth stating plainly, not hiding.** Under this fixture's
additive far-leg construction (`ConstructedBasisRule`, ADR-0025:
``far_price = near_price + basis[index]``), the calendar SPREAD itself is,
by algebraic construction, invariant to which contract is currently front:
``far_price - near_price == basis[index]`` regardless of policy. The two
compared policies therefore choose genuinely different roll dates and produce
genuinely different absolute price paths (`price_path_comparisons` below,
computed by M2's own unmodified tool), but this fixture's strategy-level
signal timing and realized P&L come out identical between them (see
`strategy_metrics_by_policy`). This is a real, structural property of the
disclosed synthetic construction used here -- not a defect in the comparison,
and not evidence that roll-method choice has no strategy effect in general. A
real second committed contract price series (not available in this
repository; see ADR-0025) would very plausibly show genuine P&L sensitivity,
since a real price is not simply an additive offset from the front
contract's own price.

Regenerate with: python3 tools/generate_roll_sensitivity_strategy_evidence.py
"""

from __future__ import annotations

import json
import sys
from datetime import date
from decimal import Decimal
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from evidence_provenance import provenance
from research.roll_method_sensitivity import compute_roll_method_strategy_sensitivity
from roll_sensitivity_fixture import build_roll_sensitivity_fixture


def _json_default(value: Any) -> Any:
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, date):
        return value.isoformat()
    raise TypeError(f"not JSON serializable: {type(value).__name__}")


def main() -> int:
    fixture = build_roll_sensitivity_fixture()
    result = compute_roll_method_strategy_sensitivity(
        fixture.chain,
        fixture.policies,
        fixture.roll_observations,
        fixture.near_prices,
        fixture.dates,
        fixture.basis_rule,
        fixture.replay_config,
    )

    price_path_differs = any(c.roll_dates_differ for c in result.price_path_comparisons)
    signal_counts = {m.signal_count for m in result.strategy_metrics_by_policy}
    pnls = {m.total_realized_pnl for m in result.strategy_metrics_by_policy}
    strategy_metrics_identical = len(signal_counts) == 1 and len(pnls) == 1

    payload = {
        **provenance(),
        "artifact": "roll_method_strategy_sensitivity",
        "requirements": ["AEGIS-024"],
        "dataset_id": "CSX-synthetic-multi-contract (tools/roll_sensitivity_fixture.py)",
        "compared_policies": sorted(fixture.policies),
        "price_path_comparisons": [
            {
                "policy_a": c.policy_a,
                "policy_b": c.policy_b,
                "roll_dates_a": [d.isoformat() for d in c.roll_dates_a],
                "roll_dates_b": [d.isoformat() for d in c.roll_dates_b],
                "roll_dates_differ": c.roll_dates_differ,
                "max_abs_price_deviation": str(c.max_abs_price_deviation),
                "mean_abs_price_deviation": str(c.mean_abs_price_deviation),
            }
            for c in result.price_path_comparisons
        ],
        "strategy_metrics_by_policy": [
            {
                "policy_name": m.policy_name,
                "signal_count": m.signal_count,
                "entry_count": m.entry_count,
                "exit_count": m.exit_count,
                "round_trip_count": m.round_trip_count,
                "total_realized_pnl": str(m.total_realized_pnl),
                "final_position": m.final_position.value,
            }
            for m in result.strategy_metrics_by_policy
        ],
        "structural_finding": {
            "price_path_differs_between_policies": price_path_differs,
            "strategy_signal_and_pnl_identical_between_policies": strategy_metrics_identical,
            "explanation": (
                "the additive far-leg basis construction makes the calendar spread "
                "algebraically invariant to which contract is currently front "
                "(far_price - near_price == basis[index] regardless of policy), so "
                "strategy-level signal timing and realized P&L come out identical "
                "between policies in THIS fixture even though roll dates and absolute "
                "price paths genuinely differ; see module docstring."
            ),
        },
        "claim": (
            "AEGIS-024: the real research.roll_method_sensitivity."
            "compute_roll_method_strategy_sensitivity was run once, over two roll "
            "policies that genuinely choose different roll dates, using the real "
            "M2 compare_roll_methods for the price-path half and the real "
            "research.strategy_replay state machine (matching "
            "cpp/participant/strategy) for the strategy half. Sensitivity is "
            "quantified honestly: roll dates and absolute price paths differ; "
            "strategy signal count and P&L do not, for the structural reason stated "
            "above. No claim is made that either roll method is universally better."
        ),
        "not_evidence_for": [
            "a claim that roll-method choice never affects strategy P&L in general -- "
            "see structural_finding.explanation",
            "any claim about real markets -- the underlying price data is "
            "synthetic/constructed throughout (ADR-0025)",
            "a claim that either compared policy is superior -- AEGIS-024 asks for "
            "sensitivity, not optimization",
        ],
    }

    out_dir = ROOT / "experiments/evidence/AEGIS-024"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "roll_method_strategy_sensitivity.json"
    out_path.write_text(
        json.dumps(payload, indent=2, sort_keys=True, default=_json_default) + "\n", encoding="utf-8"
    )
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
