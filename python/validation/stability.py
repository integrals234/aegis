"""Parameter-stability surfaces (AEGIS-142): a neighbourhood of parameter
points, not only the best one.

A report containing only the optimum is exactly the failure this
requirement exists to catch -- every evaluated grid point is stored, along
with the selected best point and a simple dispersion statistic describing
how sensitive the metric is across the neighbourhood.
"""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from statistics import pstdev

from research.calendar_spread import CalendarSpreadObservation
from research.strategy_replay import ExecutionAssumptions, ReplayConfig, replay_strategy

__all__ = [
    "GridPoint",
    "StabilitySurface",
    "compute_parameter_stability_surface",
]


@dataclass(frozen=True, slots=True)
class GridPoint:
    zscore_window: int
    entry_threshold: float
    exit_threshold: float
    total_pnl: Decimal
    round_trip_count: int


@dataclass(frozen=True, slots=True)
class StabilitySurface:
    points: tuple[GridPoint, ...]  # EVERY evaluated point, in evaluation order.
    best: GridPoint  # The single highest-total_pnl point -- reported alongside, never alone.
    metric_mean: float
    metric_stdev: float  # Population stdev of total_pnl across the whole grid -- the dispersion signal.

    def as_records(self) -> list[dict[str, object]]:
        return [
            {
                "zscore_window": p.zscore_window,
                "entry_threshold": p.entry_threshold,
                "exit_threshold": p.exit_threshold,
                "total_pnl": str(p.total_pnl),
                "round_trip_count": p.round_trip_count,
                "is_best": p == self.best,
            }
            for p in self.points
        ]


def compute_parameter_stability_surface(
    observations: tuple[CalendarSpreadObservation, ...],
    quantity_units: Decimal,
    *,
    zscore_windows: tuple[int, ...],
    entry_thresholds: tuple[float, ...],
    exit_thresholds: tuple[float, ...],
    assumptions: ExecutionAssumptions | None = None,
) -> StabilitySurface:
    """Evaluates every ``(zscore_window, entry_threshold, exit_threshold)``
    combination in the deterministic modest grid the caller supplies (kept
    small for the committed dataset's runtime -- no hyperparameter-search
    framework), skipping only combinations :class:`~research.strategy_replay.
    ReplayConfig` itself rejects as structurally invalid (exit >= entry)."""
    points: list[GridPoint] = []
    for window in zscore_windows:
        for entry in entry_thresholds:
            for exit_threshold in exit_thresholds:
                if not (0 <= exit_threshold < entry):
                    continue
                config = ReplayConfig(
                    zscore_window=window, entry_threshold=entry, exit_threshold=exit_threshold,
                    quantity_units=quantity_units,
                )
                result = replay_strategy(observations, config, assumptions)
                points.append(
                    GridPoint(
                        zscore_window=window, entry_threshold=entry, exit_threshold=exit_threshold,
                        total_pnl=result.total_pnl, round_trip_count=len(result.round_trips),
                    )
                )

    if not points:
        raise ValueError("the parameter grid produced no valid (entry, exit) combination to evaluate")

    best = max(points, key=lambda p: p.total_pnl)
    pnl_floats = [float(p.total_pnl) for p in points]
    mean = sum(pnl_floats) / len(pnl_floats)
    return StabilitySurface(
        points=tuple(points), best=best, metric_mean=mean, metric_stdev=pstdev(pnl_floats)
    )
