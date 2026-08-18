#!/usr/bin/env python3
"""Generate AEGIS-024 evidence: the M4 residual on roll-method strategy
sensitivity.

Runs the actual `research.roll_method_sensitivity.
compute_roll_method_strategy_sensitivity` -- itself a thin composition of M2's
unmodified `futures.roll_sensitivity.compare_roll_methods` and the real
`research.strategy_replay.replay_strategy` -- over two roll policies that
genuinely choose different roll dates.

**Controlled-variable design.** Every policy is run against the identical
chain, price series, date range, replay configuration and far-leg rule, and
in this fixture every policy's far leg resolves to an OBSERVED price (all
three contracts are priced). The roll policy is therefore the only variable
that differs. A comparison in which one policy's far leg were observed and
another's constructed would be confounded by provenance rather than
controlled -- which is why the fixture prices all three contracts rather than
two.

**A defect found and fixed during closure, recorded here because it changed
this artifact's result.** An earlier version of this experiment reported
*zero* strategy difference between the compared policies. That zero was an
artifact, not a finding: `build_calendar_spread_observations` always
synthesized the far-leg price from the additive `ConstructedBasisRule`, even
when the supplied price series carried that contract's own observed price.
Under a purely additive basis, ``far_price - near_price == basis[index]``
regardless of which contract is front, so the spread -- and every signal
derived from it -- was algebraically invariant to the roll policy. The
function now prefers an observed far-leg price whenever one exists and falls
back to the documented construction only when none does, recording which
happened per observation (`far_price_observed`). The invariance, and the
spurious zero it produced, are gone. Nothing was tuned to force a difference:
the fix is a provenance correction that AEGIS-076 wanted independently.

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
    metrics = result.strategy_metrics_by_policy
    pairwise = [
        {
            "policy_a": a.policy_name,
            "policy_b": b.policy_name,
            "signal_count_difference": a.signal_count - b.signal_count,
            "total_pnl_difference": str(a.total_pnl - b.total_pnl),
            "final_position_a": a.final_position.value,
            "final_position_b": b.final_position.value,
            "final_position_differs": a.final_position != b.final_position,
        }
        for i, a in enumerate(metrics)
        for b in metrics[i + 1 :]
    ]
    strategy_differs = any(
        p["total_pnl_difference"] != "0" or p["final_position_differs"] for p in pairwise
    )

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
                "contract_pairs_traded": list(m.contract_pairs),
                "signal_count": m.signal_count,
                "entry_count": m.entry_count,
                "exit_count": m.exit_count,
                "round_trip_count": m.round_trip_count,
                "total_realized_pnl": str(m.total_realized_pnl),
                "open_position_unrealized_pnl": str(m.open_position_unrealized_pnl),
                "total_pnl": str(m.total_pnl),
                "final_position": m.final_position.value,
            }
            for m in metrics
        ],
        "pairwise_strategy_differences": pairwise,
        "controlled_variable_note": (
            "every policy is run against identical chain, prices, dates, replay "
            "configuration and far-leg rule, and every policy's far leg resolves to an "
            "OBSERVED price in this fixture (all three contracts are priced) -- so the "
            "roll policy is the only variable that differs."
        ),
        "findings": {
            "price_path_differs_between_policies": price_path_differs,
            "strategy_outcome_differs_between_policies": strategy_differs,
            "explanation": (
                "the two policies select different contract pairs (see "
                "contract_pairs_traded), which produces different spread series, "
                "different signal directions and different mark-to-market P&L. Both "
                "positions remain open at the end of the observation window, so "
                "realized P&L is zero for both and the economic difference appears in "
                "open_position_unrealized_pnl / total_pnl -- which is why those are "
                "reported alongside, never folded into, the realized figure."
            ),
        },
        "claim": (
            "AEGIS-024: the real research.roll_method_sensitivity."
            "compute_roll_method_strategy_sensitivity was run once, over two roll "
            "policies that genuinely choose different roll dates, using the real "
            "M2 compare_roll_methods for the price-path half and the real "
            "research.strategy_replay state machine (matching "
            "cpp/participant/strategy) for the strategy half, with the roll policy as "
            "the only variable that differs. Strategy differences caused by roll "
            "choice are quantified above: differing traded contract pairs, differing "
            "signal direction, and differing total (mark-to-market) P&L. No claim is "
            "made that either roll method is universally better."
        ),
        "not_evidence_for": [
            "any claim about real markets -- the underlying price data is synthetic "
            "throughout (ADR-0025); the magnitude and sign of these differences are "
            "properties of this fixture, not of any real product",
            "a claim that either compared policy is superior -- AEGIS-024 asks for "
            "sensitivity, not optimization",
            "a realized trading result -- both positions remain open at the window's "
            "end; total_pnl is a mark-to-market at the final observation's own prices",
            "execution quality or fill realism -- the replay fills both legs at the "
            "observation's own price with no modelled bid/ask or slippage",
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
