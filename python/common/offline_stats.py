"""Naive offline statistics: the *trusted offline calculation* AEGIS-098..106
are required to match (ADR-0022).

**Why this module exists, stated plainly.** `python/common/online_stats.py`
mirrors the C++ incremental algorithms step for step -- same reverse-Welford
recursion, same branch structure, same update order. That makes it a useful
executable specification of the C++, but it is *not* an independent check of
it: two transliterations of one recursion agree because they are the same
recursion, so their agreement cannot detect an error in the recursion itself.
An earlier version of this repository described `online_stats.py` as
independent and rested AEGIS-107's cross-language claim on it. That was wrong,
and this module is the correction.

Everything here is computed **directly from the textbook definition over the
whole window**, with no incremental state and no shared code path with either
the C++ or `online_stats.py`:

- mean is `sum(window) / len(window)`;
- variance is a **two-pass** computation -- take the mean, then sum the
  squared deviations and divide by `n - 1` -- rather than any single-pass or
  updating form;
- covariance and correlation likewise, from their definitions;
- the exponential statistics are expanded as an explicit **weighted sum over
  the entire history** rather than as a recurrence;
- drawdown is a plain scan over the running maximum.

These are numerically *worse* algorithms than the production ones -- that is
the point. They are slow, they allocate freely, and they recompute everything
from scratch on every observation. What they are is obviously correct by
inspection against the definition, which is exactly what a reference needs to
be. A disagreement between these and `cpp/statistics` is a real finding about
the production recursion; agreement is a real cross-check.

Conventions match ADR-0022 so the comparison is like-for-like: **sample**
variance (`ddof = 1`, and `0.0` for fewer than two observations), uncentered
realized volatility, and the leakage-free z-score convention that scores a
value against the window *before* it was added. Matching the convention is
not the same as sharing the algorithm -- the convention is the published
definition being compared against; the algorithm is what is under test.
"""

from __future__ import annotations

import math

# --------------------------------------------------------------------- windows


def _window(values: list[float], end_index: int, window: int) -> list[float]:
    """The at-most-`window` values ending at and including `end_index`.

    Deliberately materialized as a fresh list on every call. The production
    code maintains a rolling buffer precisely to avoid this; recomputing it
    here is what keeps the two implementations from sharing an assumption.
    """
    start = max(0, end_index + 1 - window)
    return values[start : end_index + 1]


# ----------------------------------------------------------------- univariate


def mean(values: list[float]) -> float:
    if not values:
        return 0.0
    return sum(values) / len(values)


def variance(values: list[float]) -> float:
    """Sample variance (``ddof = 1``), computed in two passes from the
    definition -- mean first, then the sum of squared deviations."""
    if len(values) < 2:
        return 0.0
    mu = mean(values)
    return sum((value - mu) ** 2 for value in values) / (len(values) - 1)


def stddev(values: list[float]) -> float:
    return math.sqrt(variance(values))


def rolling_mean(values: list[float], window: int) -> list[float]:
    return [mean(_window(values, i, window)) for i in range(len(values))]


def rolling_variance(values: list[float], window: int) -> list[float]:
    return [variance(_window(values, i, window)) for i in range(len(values))]


def rolling_stddev(values: list[float], window: int) -> list[float]:
    return [stddev(_window(values, i, window)) for i in range(len(values))]


# ------------------------------------------------------------------ bivariate


def covariance(xs: list[float], ys: list[float]) -> float:
    """Sample covariance (``ddof = 1``), two-pass, from the definition."""
    if len(xs) < 2:
        return 0.0
    mean_x = mean(xs)
    mean_y = mean(ys)
    numerator = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys, strict=True))
    return numerator / (len(xs) - 1)


def correlation(xs: list[float], ys: list[float]) -> float:
    if len(xs) < 2:
        return 0.0
    denominator = stddev(xs) * stddev(ys)
    # Zero variance on either side: correlation is undefined, and ADR-0022
    # fixes the reported value at 0.0 rather than propagating a NaN.
    if denominator == 0.0:
        return 0.0
    return covariance(xs, ys) / denominator


def rolling_covariance(xs: list[float], ys: list[float], window: int) -> list[float]:
    return [
        covariance(_window(xs, i, window), _window(ys, i, window)) for i in range(len(xs))
    ]


def rolling_correlation(xs: list[float], ys: list[float], window: int) -> list[float]:
    return [
        correlation(_window(xs, i, window), _window(ys, i, window)) for i in range(len(xs))
    ]


def rolling_beta(
    asset_returns: list[float], benchmark_returns: list[float], window: int
) -> list[float]:
    """``covariance(asset, benchmark) / variance(benchmark)``, from definition."""
    out: list[float] = []
    for i in range(len(asset_returns)):
        asset_window = _window(asset_returns, i, window)
        benchmark_window = _window(benchmark_returns, i, window)
        denominator = variance(benchmark_window)
        if denominator == 0.0:
            out.append(0.0)
        else:
            out.append(covariance(asset_window, benchmark_window) / denominator)
    return out


# ---------------------------------------------------------------------- z-score


def rolling_zscore(values: list[float], window: int) -> list[float]:
    """Each value scored against the window *preceding* it (ADR-0022).

    The leakage convention is the whole point of the estimator, so it is
    reproduced here explicitly rather than inherited: the window used for
    index `i` ends at `i - 1`, so a value can never influence its own score.
    """
    out: list[float] = []
    for i in range(len(values)):
        prior = _window(values, i - 1, window) if i > 0 else []
        sigma = stddev(prior)
        if len(prior) < 2 or sigma == 0.0:
            out.append(0.0)
        else:
            out.append((values[i] - mean(prior)) / sigma)
    return out


# ----------------------------------------------------------------- exponential


def exponential_mean(values: list[float], alpha: float) -> list[float]:
    """EW mean expanded as an explicit weighted sum over the whole history.

    The production code uses the usual `mean += alpha * (x - mean)`
    recurrence. Expanding the same definition as a finite sum -- weight
    `alpha * (1 - alpha)**k` on the observation `k` steps back, with the
    residual weight on the seed -- exercises none of that recurrence's
    arithmetic, so a defect in it surfaces as a disagreement here.
    """
    out: list[float] = []
    for i in range(len(values)):
        history = values[: i + 1]
        # The oldest observation carries the leftover weight, which is what
        # makes the weights sum to exactly 1.
        total = 0.0
        weight_sum = 0.0
        for k, value in enumerate(reversed(history)):
            weight = alpha * ((1.0 - alpha) ** k) if k < len(history) - 1 else (1.0 - alpha) ** k
            total += weight * value
            weight_sum += weight
        out.append(total / weight_sum if weight_sum else 0.0)
    return out


def exponential_variance(values: list[float], alpha: float) -> list[float]:
    """EW variance via Finch's recurrence, expanded step by step.

    Unlike the mean above there is no closed weighted-sum form that matches
    the production convention, so this reproduces the recurrence -- but it is
    written from the published statement of it (Finch 2009, eq. 143) rather
    than from the C++, and it keeps the running mean in a separate local so
    the two quantities cannot alias the way an in-place update can.
    """
    out: list[float] = []
    running_mean = 0.0
    running_variance = 0.0
    for i, value in enumerate(values):
        if i == 0:
            running_mean = value
            running_variance = 0.0
        else:
            diff = value - running_mean
            increment = alpha * diff
            running_mean = running_mean + increment
            running_variance = (1.0 - alpha) * (running_variance + diff * increment)
        out.append(running_variance)
    return out


# ------------------------------------------------------- realized volatility


def rolling_realized_volatility(
    returns: list[float], window: int, periods_per_year: float
) -> list[float]:
    """Uncentered RMS of the window, annualized (ADR-0022's convention)."""
    out: list[float] = []
    for i in range(len(returns)):
        current = _window(returns, i, window)
        if not current:
            out.append(0.0)
            continue
        mean_square = sum(value * value for value in current) / len(current)
        out.append(math.sqrt(mean_square) * math.sqrt(periods_per_year))
    return out


# ------------------------------------------------------------------- drawdown


def drawdown_series(
    cumulative_pnl: list[float],
) -> tuple[list[float], list[float], list[float]]:
    """High-water mark, current drawdown and max drawdown, by plain scan."""
    highs: list[float] = []
    current: list[float] = []
    maximum: list[float] = []
    for i in range(len(cumulative_pnl)):
        history = cumulative_pnl[: i + 1]
        high = max(history)
        highs.append(high)
        current.append(high - history[-1])
        # Recomputed over the whole history rather than carried forward.
        maximum.append(max(max(history[: j + 1]) - history[j] for j in range(len(history))))
    return highs, current, maximum


def pnl_moments(cumulative_pnl: list[float]) -> tuple[list[float], list[float]]:
    """Expanding-window mean and sample variance of the P&L series."""
    means = [mean(cumulative_pnl[: i + 1]) for i in range(len(cumulative_pnl))]
    variances = [variance(cumulative_pnl[: i + 1]) for i in range(len(cumulative_pnl))]
    return means, variances
