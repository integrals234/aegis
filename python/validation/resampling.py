"""Bootstrap confidence intervals and Monte Carlo trade-sequence resampling
(AEGIS-146, AEGIS-147).

Both use an explicit, caller-seeded :class:`random.Random` instance --
never :func:`random.seed`/module-level RNG state -- so the same
seed and input produce byte-identical output and a different seed may
legitimately differ, exactly as the frozen acceptance requires.

The two are deliberately distinct mechanisms, not the same code under two
names: bootstrap resamples round-trip P&L WITH replacement to build a
confidence interval on a statistic (the mean); Monte Carlo resampling
PERMUTES the trade order (no replacement, every trade used exactly once
per path) to characterize path/drawdown risk, which a with-replacement
resample cannot represent (it can repeat or omit a trade, which is not
"the same trades in a different order").
"""

from __future__ import annotations

import random
from collections.abc import Sequence
from dataclasses import dataclass
from decimal import Decimal

__all__ = [
    "BootstrapResult",
    "MonteCarloPathResult",
    "MonteCarloResult",
    "bootstrap_round_trip_pnl",
    "monte_carlo_trade_resampling",
]


@dataclass(frozen=True, slots=True)
class BootstrapResult:
    statistic_name: str
    sample_unit: str
    resampling_method: str
    num_draws: int
    confidence_level: float
    seed: int
    point_estimate: float
    lower: float
    upper: float
    assumptions: str
    limitations: str


def bootstrap_round_trip_pnl(
    round_trip_pnls: Sequence[Decimal], *, num_draws: int, confidence_level: float, seed: int
) -> BootstrapResult:
    """AEGIS-146. Statistic: mean realized P&L per round trip. Sample unit:
    one completed round trip. Method: i.i.d. (not block) bootstrap -- each
    draw resamples ``len(round_trip_pnls)`` round trips WITH replacement.
    i.i.d., not block, because the sample unit is already a discrete,
    non-overlapping completed trade rather than a continuous return series
    with autocorrelation a block would need to preserve; the stated
    limitation is that this still assumes round trips are exchangeable,
    which would be false if, say, market conditions drifted systematically
    over the sample window.
    """
    if num_draws <= 0:
        raise ValueError(f"num_draws must be > 0, got {num_draws}")
    if not (0 < confidence_level < 1):
        raise ValueError(f"confidence_level must be in (0, 1), got {confidence_level}")
    values = [float(x) for x in round_trip_pnls]
    if not values:
        raise ValueError("round_trip_pnls must be non-empty")

    rng = random.Random(seed)
    n = len(values)
    draw_means = []
    for _ in range(num_draws):
        draw_means.append(sum(values[rng.randrange(n)] for _ in range(n)) / n)
    draw_means.sort()

    alpha = (1.0 - confidence_level) / 2.0
    lower_index = max(0, int(alpha * num_draws))
    upper_index = min(num_draws - 1, int((1.0 - alpha) * num_draws))

    return BootstrapResult(
        statistic_name="mean_round_trip_realized_pnl",
        sample_unit="round_trip",
        resampling_method="iid_bootstrap_with_replacement",
        num_draws=num_draws,
        confidence_level=confidence_level,
        seed=seed,
        point_estimate=sum(values) / n,
        lower=draw_means[lower_index],
        upper=draw_means[upper_index],
        assumptions="round trips are treated as exchangeable and independently drawn",
        limitations=(
            "small samples (few round trips) produce wide, unstable intervals; "
            "no serial-correlation structure between round trips is modelled"
        ),
    )


@dataclass(frozen=True, slots=True)
class MonteCarloPathResult:
    ending_pnl: float
    max_drawdown: float
    path_minimum: float


@dataclass(frozen=True, slots=True)
class MonteCarloResult:
    num_paths: int
    seed: int
    trade_count: int
    paths: tuple[MonteCarloPathResult, ...]
    ending_pnl_quantiles: dict[str, float]
    max_drawdown_quantiles: dict[str, float]


def _quantile(sorted_values: list[float], q: float) -> float:
    index = min(int(q * len(sorted_values)), len(sorted_values) - 1)
    return sorted_values[index]


def monte_carlo_trade_resampling(
    round_trip_pnls: Sequence[Decimal], *, num_paths: int, seed: int
) -> MonteCarloResult:
    """AEGIS-147: resamples the ORDER of the same trades (a permutation,
    never with-replacement) to characterize path/drawdown risk -- the same
    set of realized round trips could have occurred in a different sequence,
    and some sequences are much worse to have lived through even though the
    ending total P&L is identical for every permutation of the same
    multiset. Reports ending P&L, max drawdown and the path minimum per
    simulated path, plus 5/50/95th-percentile quantiles across paths.
    """
    if num_paths <= 0:
        raise ValueError(f"num_paths must be > 0, got {num_paths}")
    values = [float(x) for x in round_trip_pnls]
    if not values:
        raise ValueError("round_trip_pnls must be non-empty")

    rng = random.Random(seed)
    paths = []
    for _ in range(num_paths):
        order = values.copy()
        rng.shuffle(order)
        cumulative = 0.0
        peak = 0.0
        max_drawdown = 0.0
        path_minimum = 0.0
        for pnl in order:
            cumulative += pnl
            peak = max(peak, cumulative)
            max_drawdown = max(max_drawdown, peak - cumulative)
            path_minimum = min(path_minimum, cumulative)
        paths.append(MonteCarloPathResult(ending_pnl=cumulative, max_drawdown=max_drawdown, path_minimum=path_minimum))

    ending_sorted = sorted(p.ending_pnl for p in paths)
    drawdown_sorted = sorted(p.max_drawdown for p in paths)
    quantile_labels = {"p5": 0.05, "p50": 0.50, "p95": 0.95}
    return MonteCarloResult(
        num_paths=num_paths,
        seed=seed,
        trade_count=len(values),
        paths=tuple(paths),
        ending_pnl_quantiles={label: _quantile(ending_sorted, q) for label, q in quantile_labels.items()},
        max_drawdown_quantiles={label: _quantile(drawdown_sorted, q) for label, q in quantile_labels.items()},
    )
