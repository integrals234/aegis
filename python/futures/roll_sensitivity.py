"""Roll-method sensitivity, M2-owned portion (AEGIS-024).

AEGIS-024's frozen acceptance -- "one experiment report quantifies strategy
differences caused by roll choices" -- needs a strategy, which does not
exist before M4 (`docs/ROADMAP.md`). The M2-ownable half compares roll
*policies* against each other directly: which dates they choose to roll on,
and how far apart the resulting additive-adjusted price paths end up.
Strategy-level P&L comparison is the registered M4 residual
(`experiments/plans/M2.md`'s coverage table: "024 | 8 | implemented ->
M4 owner"); this module does not attempt it.

Built entirely on `python/futures/roll_audit.py` (AEGIS-023) and
`python/futures/series.py` (AEGIS-019/020) -- no roll-selection or
adjustment logic is re-derived here.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal

from futures.chain import ContractChain
from futures.identifiers import ContractId
from futures.roll.policy import RollObservation, RollPolicy
from futures.roll_audit import build_roll_audit
from futures.series import PriceObservation, build_additive_adjusted_series, build_unadjusted_series

__all__ = ["PolicyComparison", "compare_roll_methods"]


@dataclass(frozen=True, slots=True)
class PolicyComparison:
    """How two roll policies' choices diverge over the same chain,
    observations and date range."""

    policy_a: str
    policy_b: str
    roll_dates_a: tuple[date, ...]
    roll_dates_b: tuple[date, ...]
    roll_dates_differ: bool
    max_abs_price_deviation: Decimal
    mean_abs_price_deviation: Decimal


def compare_roll_methods(
    chain: ContractChain,
    policies: Mapping[str, RollPolicy],
    observations: Sequence[RollObservation],
    prices: Sequence[PriceObservation],
    dates: Sequence[date],
) -> tuple[PolicyComparison, ...]:
    """Pairwise comparison of roll dates and additive-adjusted price paths
    across ``policies``. Every pair is compared once, in the policies'
    sorted-name order, so the result does not depend on dict iteration or
    insertion order.
    """
    roll_dates_by_policy: dict[str, tuple[date, ...]] = {}
    adjusted_price_by_policy: dict[str, dict[date, Decimal]] = {}

    for name, policy in policies.items():
        front_by_date: dict[date, ContractId] = {}
        for as_of in dates:
            front = policy.front_contract(chain, observations, as_of)
            if front is not None:
                front_by_date[as_of] = front
        if len(front_by_date) != len(dates):
            raise ValueError(f"policy {name!r} has no listed contract on at least one date in range")

        unadjusted = build_unadjusted_series(front_by_date, prices)
        adjusted = build_additive_adjusted_series(unadjusted, prices)
        audit = build_roll_audit(chain, policy, observations, prices, dates)

        roll_dates_by_policy[name] = tuple(record.as_of for record in audit)
        adjusted_price_by_policy[name] = {obs.as_of: obs.adjusted_price for obs in adjusted}

    comparisons: list[PolicyComparison] = []
    names = sorted(policies)
    for i, name_a in enumerate(names):
        for name_b in names[i + 1 :]:
            common_dates = sorted(set(adjusted_price_by_policy[name_a]) & set(adjusted_price_by_policy[name_b]))
            deviations = [
                abs(adjusted_price_by_policy[name_a][d] - adjusted_price_by_policy[name_b][d])
                for d in common_dates
            ]
            max_deviation = max(deviations) if deviations else Decimal(0)
            mean_deviation = (sum(deviations, Decimal(0)) / len(deviations)) if deviations else Decimal(0)
            comparisons.append(
                PolicyComparison(
                    policy_a=name_a,
                    policy_b=name_b,
                    roll_dates_a=roll_dates_by_policy[name_a],
                    roll_dates_b=roll_dates_by_policy[name_b],
                    roll_dates_differ=roll_dates_by_policy[name_a] != roll_dates_by_policy[name_b],
                    max_abs_price_deviation=max_deviation,
                    mean_abs_price_deviation=mean_deviation,
                )
            )
    return tuple(comparisons)
