"""Formal strategy rejection report (AEGIS-155): a machine-readable ACCEPT
or REJECT verdict, with the specific criteria that triggered it.

Every criterion is evaluated and recorded, whether or not it triggers --
the report never lists only the reasons for the verdict it reached. Reuses
this milestone's own other validation outputs (cost sensitivity, the
stability surface, the bootstrap CI) rather than re-deriving them.

The five categories `experiments/plans/M5.md` names, and how each is read
here where the research/validation layer can actually evaluate it (some
apply at the portfolio/risk level, AEGIS-134, and are not recomputed here):

* transaction costs -- unprofitable even at the lowest swept cost level;
* parameter instability -- high dispersion of the metric across the
  stability surface's neighbourhood, relative to its own scale;
* concentration -- read at the research layer as TRADE concentration: too
  few round trips for the other statistics to be trustworthy, not
  portfolio instrument concentration (AEGIS-134's own concern);
* statistical/test failure -- the bootstrap confidence interval for mean
  round-trip P&L does not exclude a non-positive mean;
* drawdown -- the strategy's own realized trade sequence's maximum
  drawdown exceeds a documented threshold.
"""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from enum import StrEnum

from research.strategy_replay import StrategyReplayResult

from validation.resampling import BootstrapResult
from validation.sensitivity import CostSweepResult
from validation.stability import StabilitySurface

__all__ = [
    "RejectionCriterion",
    "RejectionReport",
    "RejectionVerdict",
    "evaluate_strategy_for_rejection",
    "realized_trade_sequence_max_drawdown",
]


class RejectionVerdict(StrEnum):
    ACCEPT = "accept"
    REJECT = "reject"


@dataclass(frozen=True, slots=True)
class RejectionCriterion:
    name: str
    triggered: bool
    detail: str


@dataclass(frozen=True, slots=True)
class RejectionReport:
    verdict: RejectionVerdict
    criteria: tuple[RejectionCriterion, ...]  # EVERY evaluated criterion, triggered or not.

    @property
    def triggering_criteria(self) -> tuple[RejectionCriterion, ...]:
        return tuple(c for c in self.criteria if c.triggered)

    def as_record(self) -> dict[str, object]:
        return {
            "verdict": self.verdict.value,
            "criteria": [{"name": c.name, "triggered": c.triggered, "detail": c.detail} for c in self.criteria],
        }


def realized_trade_sequence_max_drawdown(result: StrategyReplayResult) -> Decimal:
    """Maximum peak-to-trough drawdown of the ACTUAL realized round-trip
    sequence, in the order the trades really occurred -- distinct from
    `validation.resampling`'s Monte Carlo drawdown distribution, which
    resamples that order. This is the one observed path, not a simulation."""
    cumulative = Decimal(0)
    peak = Decimal(0)
    max_drawdown = Decimal(0)
    for round_trip in result.round_trips:
        cumulative += round_trip.realized_pnl
        peak = max(peak, cumulative)
        max_drawdown = max(max_drawdown, peak - cumulative)
    return max_drawdown


def evaluate_strategy_for_rejection(
    result: StrategyReplayResult,
    *,
    cost_sensitivity: CostSweepResult | None = None,
    stability: StabilitySurface | None = None,
    bootstrap: BootstrapResult | None = None,
    max_drawdown_threshold: Decimal | None = None,
    min_round_trip_count: int = 5,
    instability_coefficient_of_variation_threshold: float = 2.0,
) -> RejectionReport:
    criteria: list[RejectionCriterion] = []

    if cost_sensitivity is not None:
        triggered = cost_sensitivity.break_even_index == 0
        criteria.append(
            RejectionCriterion(
                name="transaction_cost_unprofitable_at_lowest_swept_cost",
                triggered=triggered,
                detail=f"break_even_index={cost_sensitivity.break_even_index}",
            )
        )

    if stability is not None:
        coefficient_of_variation = (
            abs(stability.metric_stdev / stability.metric_mean) if stability.metric_mean != 0 else float("inf")
        )
        triggered = coefficient_of_variation > instability_coefficient_of_variation_threshold
        criteria.append(
            RejectionCriterion(
                name="parameter_instability",
                triggered=triggered,
                detail=(
                    f"coefficient_of_variation={coefficient_of_variation:.4f} "
                    f"threshold={instability_coefficient_of_variation_threshold}"
                ),
            )
        )

    round_trip_count = len(result.round_trips)
    criteria.append(
        RejectionCriterion(
            name="trade_concentration_too_few_round_trips",
            triggered=round_trip_count < min_round_trip_count,
            detail=f"round_trip_count={round_trip_count} min_required={min_round_trip_count}",
        )
    )

    if bootstrap is not None:
        triggered = bootstrap.upper <= 0
        criteria.append(
            RejectionCriterion(
                name="statistical_test_failure_ci_excludes_no_positive_mean",
                triggered=triggered,
                detail=f"bootstrap_ci=[{bootstrap.lower:.4f}, {bootstrap.upper:.4f}]",
            )
        )

    if max_drawdown_threshold is not None:
        observed_drawdown = realized_trade_sequence_max_drawdown(result)
        criteria.append(
            RejectionCriterion(
                name="max_drawdown_exceeds_threshold",
                triggered=observed_drawdown > max_drawdown_threshold,
                detail=f"observed={observed_drawdown} threshold={max_drawdown_threshold}",
            )
        )

    verdict = RejectionVerdict.REJECT if any(c.triggered for c in criteria) else RejectionVerdict.ACCEPT
    return RejectionReport(verdict=verdict, criteria=tuple(criteria))
