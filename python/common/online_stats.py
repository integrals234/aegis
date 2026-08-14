"""Independent Python reference for the online statistics estimators
(AEGIS-098..107; ADR-0022).

This is the reference AEGIS-107's cross-language comparison holds the C++
production estimators (`cpp/statistics`) to. It is written from this
module's own reading of the mathematics, not by porting the C++ — an
implementation cannot validate itself, so agreement between two independent
readings is what "cross-language validation" is allowed to mean here.

Every class below mirrors the numerical convention ADR-0022 fixed once, in
`cpp/statistics`, and restates here rather than reinventing:

* sample statistics (``ddof = 1``), not population statistics;
* reverse-Welford (or its bivariate/exponential analogue) for numerically
  stable sliding-window updates;
* the documented edge cases — fewer than two observations, zero variance,
  a constant series, a first observation, an empty window — return ``0.0``
  rather than raising or propagating ``nan``.

Dependency-light on purpose: standard library only (``collections.deque``,
``math``), so nothing about this reference's own correctness depends on a
third-party numerical library's floating-point choices — the exact kind of
external variable a cross-language comparison is supposed to rule out.
"""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field


@dataclass
class RollingMoments:
    """Fixed-window mean, sample variance and standard deviation
    (AEGIS-098, AEGIS-099, AEGIS-100)."""

    window: int
    _buffer: deque[float] = field(init=False)
    _mean: float = field(default=0.0, init=False)
    _m2: float = field(default=0.0, init=False)

    def __post_init__(self) -> None:
        if self.window <= 0:
            raise ValueError("window must be positive")
        self._buffer = deque()

    def push(self, value: float) -> None:
        if len(self._buffer) == self.window:
            evicted = self._buffer.popleft()
            n_before = len(self._buffer) + 1
            n_after = len(self._buffer)
            if n_after == 0:
                self._mean = 0.0
                self._m2 = 0.0
            else:
                mean_prev = (n_before * self._mean - evicted) / n_after
                self._m2 -= (evicted - mean_prev) * (evicted - self._mean)
                self._mean = mean_prev

        self._buffer.append(value)
        n = len(self._buffer)
        delta = value - self._mean
        self._mean += delta / n
        delta2 = value - self._mean
        self._m2 += delta * delta2

    def count(self) -> int:
        return len(self._buffer)

    def mean(self) -> float:
        return self._mean

    def variance(self) -> float:
        if len(self._buffer) < 2:
            return 0.0
        return self._m2 / (len(self._buffer) - 1)

    def stddev(self) -> float:
        return math.sqrt(self.variance())


@dataclass
class RollingCovariance:
    """Fixed-window covariance/correlation between two paired series
    (AEGIS-101, AEGIS-102), via the bivariate Welford recursion."""

    window: int
    _buffer: deque[tuple[float, float]] = field(init=False)
    _mean_x: float = field(default=0.0, init=False)
    _mean_y: float = field(default=0.0, init=False)
    _m2_x: float = field(default=0.0, init=False)
    _m2_y: float = field(default=0.0, init=False)
    _c_xy: float = field(default=0.0, init=False)

    def __post_init__(self) -> None:
        if self.window <= 0:
            raise ValueError("window must be positive")
        self._buffer = deque()

    def push(self, x: float, y: float) -> None:
        if len(self._buffer) == self.window:
            evicted_x, evicted_y = self._buffer.popleft()
            n_before = len(self._buffer) + 1
            n_after = len(self._buffer)
            if n_after == 0:
                self._mean_x = 0.0
                self._mean_y = 0.0
                self._m2_x = 0.0
                self._m2_y = 0.0
                self._c_xy = 0.0
            else:
                mean_x_prev = (n_before * self._mean_x - evicted_x) / n_after
                mean_y_prev = (n_before * self._mean_y - evicted_y) / n_after
                self._m2_x -= (evicted_x - mean_x_prev) * (evicted_x - self._mean_x)
                self._m2_y -= (evicted_y - mean_y_prev) * (evicted_y - self._mean_y)
                self._c_xy -= (evicted_x - mean_x_prev) * (evicted_y - self._mean_y)
                self._mean_x = mean_x_prev
                self._mean_y = mean_y_prev

        self._buffer.append((x, y))
        n = len(self._buffer)
        dx = x - self._mean_x
        dy = y - self._mean_y
        self._mean_x += dx / n
        self._mean_y += dy / n
        dx2 = x - self._mean_x
        dy2 = y - self._mean_y
        self._m2_x += dx * dx2
        self._m2_y += dy * dy2
        self._c_xy += dx * dy2

    def count(self) -> int:
        return len(self._buffer)

    def mean_x(self) -> float:
        return self._mean_x

    def mean_y(self) -> float:
        return self._mean_y

    def variance_x(self) -> float:
        if len(self._buffer) < 2:
            return 0.0
        return self._m2_x / (len(self._buffer) - 1)

    def variance_y(self) -> float:
        if len(self._buffer) < 2:
            return 0.0
        return self._m2_y / (len(self._buffer) - 1)

    def covariance(self) -> float:
        if len(self._buffer) < 2:
            return 0.0
        return self._c_xy / (len(self._buffer) - 1)

    def correlation(self) -> float:
        denominator = math.sqrt(self.variance_x() * self.variance_y())
        if denominator <= 0.0:
            return 0.0
        return self.covariance() / denominator


@dataclass
class RollingZScore:
    """Leakage-free rolling z-score (AEGIS-103): scores against the *prior*
    window, then adds the value — an observation never influences its own
    normalisation."""

    window: int
    _moments: RollingMoments = field(init=False)

    def __post_init__(self) -> None:
        self._moments = RollingMoments(self.window)

    def push_and_score(self, value: float) -> float:
        prior_mean = self._moments.mean()
        prior_stddev = self._moments.stddev()
        score = (value - prior_mean) / prior_stddev if prior_stddev > 0.0 else 0.0
        self._moments.push(value)
        return score

    def count(self) -> int:
        return self._moments.count()


@dataclass
class ExponentialStats:
    """Exponentially weighted mean and variance (AEGIS-104), per Finch
    (2009), "Incremental Calculation of Weighted Mean and Variance."
    ``alpha`` is the smoothing factor applied to the newest observation
    directly."""

    alpha: float
    _initialized: bool = field(default=False, init=False)
    _mean: float = field(default=0.0, init=False)
    _variance: float = field(default=0.0, init=False)

    def __post_init__(self) -> None:
        if not (0.0 < self.alpha <= 1.0):
            raise ValueError("alpha must be in (0, 1]")

    def push(self, value: float) -> None:
        if not self._initialized:
            self._mean = value
            self._variance = 0.0
            self._initialized = True
            return
        diff = value - self._mean
        incr = self.alpha * diff
        self._mean += incr
        self._variance = (1.0 - self.alpha) * (self._variance + diff * incr)

    def has_value(self) -> bool:
        return self._initialized

    def mean(self) -> float:
        return self._mean

    def variance(self) -> float:
        return self._variance

    def stddev(self) -> float:
        return math.sqrt(self._variance)


@dataclass
class RollingRealizedVolatility:
    """Root-mean-square of returns in a fixed window (AEGIS-105) —
    uncentered, the standard convention for high-frequency returns assumed
    close to zero-mean."""

    window: int
    _buffer: deque[float] = field(init=False)
    _sum_squares: float = field(default=0.0, init=False)

    def __post_init__(self) -> None:
        if self.window <= 0:
            raise ValueError("window must be positive")
        self._buffer = deque()

    def push(self, return_value: float) -> None:
        if len(self._buffer) == self.window:
            evicted = self._buffer.popleft()
            self._sum_squares -= evicted * evicted
        self._buffer.append(return_value)
        self._sum_squares += return_value * return_value

    def count(self) -> int:
        return len(self._buffer)

    def realized_volatility(self, periods_per_year: float = 1.0) -> float:
        if not self._buffer:
            return 0.0
        mean_square = self._sum_squares / len(self._buffer)
        return math.sqrt(mean_square) * math.sqrt(periods_per_year)


@dataclass
class RollingBeta:
    """``beta = covariance(asset, benchmark) / variance(benchmark)``
    (AEGIS-105), built on `RollingCovariance` rather than duplicating its
    recursion."""

    window: int
    _covariance: RollingCovariance = field(init=False)

    def __post_init__(self) -> None:
        self._covariance = RollingCovariance(self.window)

    def push(self, asset_return: float, benchmark_return: float) -> None:
        self._covariance.push(asset_return, benchmark_return)

    def count(self) -> int:
        return self._covariance.count()

    def beta(self) -> float:
        benchmark_variance = self._covariance.variance_y()
        if benchmark_variance <= 0.0:
            return 0.0
        return self._covariance.covariance() / benchmark_variance


@dataclass
class DrawdownTracker:
    """Online high-water mark, drawdown, and expanding-window mean/variance
    of a cumulative value series (AEGIS-106). Expanding, not sliding: a
    high-water mark is an all-time quantity by definition."""

    _count: int = field(default=0, init=False)
    _mean: float = field(default=0.0, init=False)
    _m2: float = field(default=0.0, init=False)
    _initialized: bool = field(default=False, init=False)
    _high_water_mark: float = field(default=0.0, init=False)
    _current_drawdown: float = field(default=0.0, init=False)
    _max_drawdown: float = field(default=0.0, init=False)

    def push(self, value: float) -> None:
        self._count += 1
        n = self._count
        delta = value - self._mean
        self._mean += delta / n
        delta2 = value - self._mean
        self._m2 += delta * delta2

        if not self._initialized or value > self._high_water_mark:
            self._high_water_mark = value
            self._initialized = True
        self._current_drawdown = self._high_water_mark - value
        if self._current_drawdown > self._max_drawdown:
            self._max_drawdown = self._current_drawdown

    def count(self) -> int:
        return self._count

    def mean(self) -> float:
        return self._mean

    def variance(self) -> float:
        if self._count < 2:
            return 0.0
        return self._m2 / (self._count - 1)

    def high_water_mark(self) -> float:
        return self._high_water_mark

    def current_drawdown(self) -> float:
        return self._current_drawdown

    def max_drawdown(self) -> float:
        return self._max_drawdown
