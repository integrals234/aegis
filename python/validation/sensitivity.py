"""Transaction-cost, latency and slippage/fill-assumption sensitivity
(AEGIS-143, AEGIS-144, AEGIS-145).

Each sweep varies exactly one :class:`~research.strategy_replay.
ExecutionAssumptions` dimension (or, for AEGIS-145, two: slippage and the
discrete fill assumption) while holding everything else fixed, and reports
the strategy's outcome at every swept level -- never only the extremes.
"""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal

from research.calendar_spread import CalendarSpreadObservation
from research.strategy_replay import ExecutionAssumptions, FillAssumption, ReplayConfig, replay_strategy

__all__ = [
    "CostSweepPoint",
    "CostSweepResult",
    "FillSweepPoint",
    "FillSweepResult",
    "LatencySweepPoint",
    "LatencySweepResult",
    "compute_latency_sensitivity",
    "compute_slippage_and_fill_sensitivity",
    "compute_transaction_cost_sensitivity",
]


@dataclass(frozen=True, slots=True)
class CostSweepPoint:
    fee_per_unit: Decimal
    half_spread: Decimal
    slippage_per_unit: Decimal
    total_pnl: Decimal
    round_trip_count: int


@dataclass(frozen=True, slots=True)
class CostSweepResult:
    points: tuple[CostSweepPoint, ...]  # In ascending cost order.
    break_even_index: int | None  # First index where total_pnl <= 0; None if it never crosses.

    def as_records(self) -> list[dict[str, object]]:
        return [
            {
                "fee_per_unit": str(p.fee_per_unit),
                "half_spread": str(p.half_spread),
                "slippage_per_unit": str(p.slippage_per_unit),
                "total_pnl": str(p.total_pnl),
                "round_trip_count": p.round_trip_count,
            }
            for p in self.points
        ]


def compute_transaction_cost_sensitivity(
    observations: tuple[CalendarSpreadObservation, ...],
    config: ReplayConfig,
    *,
    cost_levels: tuple[Decimal, ...],
) -> CostSweepResult:
    """AEGIS-143. ``cost_levels`` is applied identically to fee, half-spread
    and slippage at each swept level (a single "cost multiple" scale, the
    narrowest defensible sweep dimension for three cost fields that all act
    additively on the same per-transaction charge). Reports break-even where
    the committed data actually crosses it; if it never does, that absence
    is itself the honest finding -- never invented."""
    points = []
    for level in cost_levels:
        assumptions = ExecutionAssumptions(fee_per_unit=level, half_spread=level, slippage_per_unit=level)
        result = replay_strategy(observations, config, assumptions)
        points.append(
            CostSweepPoint(
                fee_per_unit=level, half_spread=level, slippage_per_unit=level,
                total_pnl=result.total_pnl, round_trip_count=len(result.round_trips),
            )
        )
    break_even_index = next((i for i, p in enumerate(points) if p.total_pnl <= 0), None)
    return CostSweepResult(points=tuple(points), break_even_index=break_even_index)


@dataclass(frozen=True, slots=True)
class LatencySweepPoint:
    decision_delay_days: int
    execution_delay_days: int
    total_pnl: Decimal
    round_trip_count: int
    dropped_signal_count: int


@dataclass(frozen=True, slots=True)
class LatencySweepResult:
    points: tuple[LatencySweepPoint, ...]

    def as_records(self) -> list[dict[str, object]]:
        return [
            {
                "decision_delay_days": p.decision_delay_days,
                "execution_delay_days": p.execution_delay_days,
                "total_pnl": str(p.total_pnl),
                "round_trip_count": p.round_trip_count,
                "dropped_signal_count": p.dropped_signal_count,
            }
            for p in self.points
        ]


def compute_latency_sensitivity(
    observations: tuple[CalendarSpreadObservation, ...],
    config: ReplayConfig,
    *,
    decision_delays: tuple[int, ...],
    execution_delays: tuple[int, ...],
) -> LatencySweepResult:
    """AEGIS-144. Each swept delay pair is a REAL
    :class:`~research.strategy_replay.ExecutionAssumptions` fed through
    :func:`~research.strategy_replay.replay_strategy`, which uses it to
    shift the execution index on the observation grid (module docstring) --
    fills genuinely move or drop, they are not a number subtracted from a
    report afterward."""
    points = []
    for decision_delay in decision_delays:
        for execution_delay in execution_delays:
            assumptions = ExecutionAssumptions(
                decision_delay_days=decision_delay, execution_delay_days=execution_delay
            )
            result = replay_strategy(observations, config, assumptions)
            points.append(
                LatencySweepPoint(
                    decision_delay_days=decision_delay, execution_delay_days=execution_delay,
                    total_pnl=result.total_pnl, round_trip_count=len(result.round_trips),
                    dropped_signal_count=result.dropped_signal_count,
                )
            )
    return LatencySweepResult(points=tuple(points))


@dataclass(frozen=True, slots=True)
class FillSweepPoint:
    slippage_per_unit: Decimal
    fill_assumption: FillAssumption
    total_pnl: Decimal
    round_trip_count: int
    dropped_signal_count: int


@dataclass(frozen=True, slots=True)
class FillSweepResult:
    points: tuple[FillSweepPoint, ...]

    def as_records(self) -> list[dict[str, object]]:
        return [
            {
                "slippage_per_unit": str(p.slippage_per_unit),
                "fill_assumption": p.fill_assumption.value,
                "total_pnl": str(p.total_pnl),
                "round_trip_count": p.round_trip_count,
                "dropped_signal_count": p.dropped_signal_count,
            }
            for p in self.points
        ]


def compute_slippage_and_fill_sensitivity(
    observations: tuple[CalendarSpreadObservation, ...],
    config: ReplayConfig,
    *,
    slippage_levels: tuple[Decimal, ...],
) -> FillSweepResult:
    """AEGIS-145: sweeps BOTH slippage and fill assumption, as the frozen
    acceptance requires. Every ``(slippage_level, fill_assumption)`` pair is
    a genuine replay under that ``ExecutionAssumptions``, so the reported
    ``round_trip_count``/``dropped_signal_count`` differences between
    ``TOUCH`` and ``CROSS_OR_NEXT`` reflect a real change in fill
    eligibility, not a label."""
    points = []
    for slippage in slippage_levels:
        for fill_assumption in (FillAssumption.TOUCH, FillAssumption.CROSS_OR_NEXT):
            assumptions = ExecutionAssumptions(slippage_per_unit=slippage, fill_assumption=fill_assumption)
            result = replay_strategy(observations, config, assumptions)
            points.append(
                FillSweepPoint(
                    slippage_per_unit=slippage, fill_assumption=fill_assumption,
                    total_pnl=result.total_pnl, round_trip_count=len(result.round_trips),
                    dropped_signal_count=result.dropped_signal_count,
                )
            )
    return FillSweepResult(points=tuple(points))
