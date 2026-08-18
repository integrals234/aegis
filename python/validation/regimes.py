"""Regime-specific evaluation (AEGIS-149): every configured regime must
appear in the report, including one with zero trades -- never silently
dropped.

Regimes are defined by simple, deterministic, config-driven date ranges
(``configs/validation/regimes.yaml``), not by an adaptive detector -- this
is validation reporting, not the regime-aware strategy logic M6 owns.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date
from pathlib import Path

import yaml
from research.calendar_spread import CalendarSpreadObservation
from research.strategy_replay import ExecutionAssumptions, ReplayConfig, StrategyReplayResult, replay_strategy

__all__ = [
    "RegimeDefinition",
    "RegimeReport",
    "RegimeResult",
    "load_regime_definitions",
    "run_regime_evaluation",
]


@dataclass(frozen=True, slots=True)
class RegimeDefinition:
    name: str
    start: date
    end: date  # Inclusive.

    def __post_init__(self) -> None:
        if self.start > self.end:
            raise ValueError(f"regime {self.name!r}: start {self.start} is after end {self.end}")

    def contains(self, as_of: date) -> bool:
        return self.start <= as_of <= self.end


def load_regime_definitions(repo_root: Path) -> tuple[RegimeDefinition, ...]:
    path = repo_root / "configs" / "validation" / "regimes.yaml"
    doc = yaml.safe_load(path.read_text())
    return tuple(
        RegimeDefinition(name=entry["name"], start=entry["start"], end=entry["end"])
        for entry in doc["regimes"]
    )


@dataclass(frozen=True, slots=True)
class RegimeResult:
    name: str
    observation_count: int
    result: StrategyReplayResult


@dataclass(frozen=True, slots=True)
class RegimeReport:
    regimes: tuple[RegimeResult, ...]

    def as_records(self) -> list[dict[str, object]]:
        return [
            {
                "regime": r.name,
                "observation_count": r.observation_count,
                "round_trip_count": len(r.result.round_trips),
                "total_pnl": str(r.result.total_pnl),
            }
            for r in self.regimes
        ]


def run_regime_evaluation(
    observations: tuple[CalendarSpreadObservation, ...],
    regimes: tuple[RegimeDefinition, ...],
    replay_config: ReplayConfig,
    assumptions: ExecutionAssumptions | None = None,
) -> RegimeReport:
    """Every ``regimes`` entry produces exactly one :class:`RegimeResult`,
    even if no observation falls inside it (``observation_count == 0``,
    ``result`` from an empty replay) -- a regime a dataset happens not to
    cover is a real finding, not something this function may omit.

    Each regime is replayed as its own self-contained window: the rolling
    z-score resets at the regime's start rather than carrying state across a
    boundary. Documented, deliberate simplification -- "regime-specific
    evaluation" reads most naturally as "how does the strategy behave
    *within* this regime", and letting the signal window straddle a regime
    boundary would let the adjacent regime's data silently influence the
    result being attributed to this one.
    """
    results = []
    for regime in regimes:
        subset = tuple(o for o in observations if regime.contains(o.as_of))
        result = replay_strategy(subset, replay_config, assumptions)
        results.append(RegimeResult(name=regime.name, observation_count=len(subset), result=result))
    return RegimeReport(regimes=tuple(results))
