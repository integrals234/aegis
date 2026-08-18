"""Multiple-market validation (AEGIS-148): a report must not rely on one
cherry-picked instrument.

The three product families this repository has configured
(``configs/futures/products.yaml``, verified below rather than hardcoded
from memory) are EQX (equity index), CLX (energy) and SRX. All three are
synthetic (``venue: SYNX``); this module makes no claim of generalization
beyond them (ADR-0025's disclosure convention).
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import yaml
from research.strategy_replay import ExecutionAssumptions, ReplayConfig, StrategyReplayResult, replay_strategy

from validation._fixtures import make_synthetic_spread_series

__all__ = [
    "MarketResult",
    "MultiMarketReport",
    "configured_product_roots",
    "run_multi_market_validation",
]


def configured_product_roots(repo_root: Path) -> tuple[str, ...]:
    """Reads the actual product families from
    ``configs/futures/products.yaml`` -- never a hardcoded name list, so a
    future addition or removal of a product family is picked up
    automatically rather than silently skipped."""
    doc = yaml.safe_load((repo_root / "configs" / "futures" / "products.yaml").read_text())
    return tuple(product["product_root"] for product in doc["products"])


@dataclass(frozen=True, slots=True)
class MarketResult:
    product_root: str
    seed: int
    result: StrategyReplayResult


@dataclass(frozen=True, slots=True)
class MultiMarketReport:
    markets: tuple[MarketResult, ...]

    def as_records(self) -> list[dict[str, object]]:
        """Every configured market appears here, including a weak or
        zero-trade outcome -- never silently dropped because it looked
        uninteresting (the frozen acceptance this report exists to satisfy)."""
        return [
            {
                "product_root": m.product_root,
                "seed": m.seed,
                "round_trip_count": len(m.result.round_trips),
                "total_pnl": str(m.result.total_pnl),
                "entry_count": m.result.entry_count,
            }
            for m in self.markets
        ]


def run_multi_market_validation(
    product_roots: tuple[str, ...],
    replay_config: ReplayConfig,
    *,
    base_seed: int,
    num_days: int = 120,
    assumptions: ExecutionAssumptions | None = None,
) -> MultiMarketReport:
    """Runs the identical strategy/replay methodology across every market in
    ``product_roots``, each on its own deterministically seeded series
    (``base_seed + index``, so markets do not share a random stream and
    a caller can still reproduce any one of them from the recorded seed)."""
    markets = []
    for index, product_root in enumerate(product_roots):
        seed = base_seed + index
        observations = make_synthetic_spread_series(product_root, num_days=num_days, seed=seed)
        result = replay_strategy(observations, replay_config, assumptions)
        markets.append(MarketResult(product_root=product_root, seed=seed, result=result))
    return MultiMarketReport(markets=tuple(markets))
