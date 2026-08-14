"""AEGIS-098..106 — Python reference matches trusted offline calculations
and the documented edge cases (ADR-0022)."""

from __future__ import annotations

import math

import pytest
from common.online_stats import (
    DrawdownTracker,
    ExponentialStats,
    RollingBeta,
    RollingCovariance,
    RollingMoments,
    RollingRealizedVolatility,
    RollingZScore,
)

pytestmark = pytest.mark.unit


def _offline_mean(values: list[float]) -> float:
    return sum(values) / len(values)


def _offline_variance(values: list[float]) -> float:
    if len(values) < 2:
        return 0.0
    mean = _offline_mean(values)
    return sum((v - mean) ** 2 for v in values) / (len(values) - 1)


def test_rolling_moments_matches_offline_while_sliding_past_the_window():
    window = 4
    moments = RollingMoments(window)
    buffer: list[float] = []
    for value in [1.0, 3.0, 5.0, 7.0, 9.0, 2.0, 8.0]:
        moments.push(value)
        buffer.append(value)
        if len(buffer) > window:
            buffer.pop(0)
        assert moments.mean() == pytest.approx(_offline_mean(buffer))
        assert moments.variance() == pytest.approx(_offline_variance(buffer))


def test_rolling_moments_edge_cases():
    moments = RollingMoments(3)
    assert moments.mean() == 0.0
    assert moments.variance() == 0.0
    moments.push(42.0)
    assert moments.mean() == 42.0
    assert moments.variance() == 0.0


def test_rolling_moments_rejects_nonpositive_window():
    with pytest.raises(ValueError, match="window"):
        RollingMoments(0)


def test_rolling_covariance_matches_offline_after_sliding():
    window = 4
    cov = RollingCovariance(window)
    buffer: list[tuple[float, float]] = []
    series = [(1.0, 2.0), (2.0, 3.5), (3.0, 3.0), (4.0, 5.0), (5.0, 4.0), (6.0, 7.0)]
    for x, y in series:
        cov.push(x, y)
        buffer.append((x, y))
        if len(buffer) > window:
            buffer.pop(0)
        if len(buffer) < 2:
            assert cov.covariance() == 0.0
            continue
        mean_x = _offline_mean([p[0] for p in buffer])
        mean_y = _offline_mean([p[1] for p in buffer])
        expected = sum((px - mean_x) * (py - mean_y) for px, py in buffer) / (len(buffer) - 1)
        assert cov.covariance() == pytest.approx(expected)


def test_rolling_correlation_perfect_and_constant_edge_case():
    cov = RollingCovariance(5)
    for v in range(1, 6):
        cov.push(float(v), 2.0 * v + 3.0)
    assert cov.correlation() == pytest.approx(1.0)

    constant = RollingCovariance(5)
    for i in range(5):
        constant.push(3.0, float(i))
    assert constant.correlation() == 0.0  # x never moves: defined zero, not NaN.


def test_rolling_zscore_is_leakage_free():
    z = RollingZScore(3)
    assert z.push_and_score(42.0) == 0.0  # No prior window.
    z.push_and_score(1.0)
    z.push_and_score(2.0)
    z.push_and_score(3.0)
    # Prior window {1,2,3}: mean 2, sample stddev 1.
    score = z.push_and_score(10.0)
    assert score == pytest.approx((10.0 - 2.0) / 1.0)


def test_rolling_zscore_constant_prior_window_edge_case():
    z = RollingZScore(3)
    z.push_and_score(5.0)
    score = z.push_and_score(5.0)
    assert score == 0.0


def test_exponential_stats_matches_hand_computed_recursion():
    stats = ExponentialStats(0.5)
    assert not stats.has_value()
    stats.push(10.0)
    assert stats.mean() == 10.0
    assert stats.variance() == 0.0
    stats.push(20.0)
    assert stats.mean() == pytest.approx(15.0)
    assert stats.variance() == pytest.approx(25.0)
    stats.push(10.0)
    assert stats.mean() == pytest.approx(12.5)
    assert stats.variance() == pytest.approx(18.75)


def test_exponential_stats_rejects_out_of_range_alpha():
    with pytest.raises(ValueError, match="alpha"):
        ExponentialStats(0.0)
    with pytest.raises(ValueError, match="alpha"):
        ExponentialStats(1.5)


def test_realized_volatility_matches_offline_root_mean_square():
    window = 4
    vol = RollingRealizedVolatility(window)
    buffer: list[float] = []
    for r in [0.01, -0.02, 0.015, -0.005, 0.03, -0.01]:
        vol.push(r)
        buffer.append(r)
        if len(buffer) > window:
            buffer.pop(0)
        expected = math.sqrt(sum(v * v for v in buffer) / len(buffer))
        assert vol.realized_volatility() == pytest.approx(expected)


def test_realized_volatility_annualization():
    vol = RollingRealizedVolatility(4)
    vol.push(0.01)
    vol.push(-0.01)
    assert vol.realized_volatility(252.0) == pytest.approx(
        vol.realized_volatility() * math.sqrt(252.0)
    )


def test_rolling_beta_matches_expected_slope():
    beta = RollingBeta(5)
    for b in range(1, 6):
        beta.push(2.0 * b, float(b))
    assert beta.beta() == pytest.approx(2.0)


def test_rolling_beta_zero_benchmark_variance_edge_case():
    beta = RollingBeta(5)
    for i in range(5):
        beta.push(float(i), 3.0)
    assert beta.beta() == 0.0


def test_drawdown_tracker_tracks_peak_and_retreat():
    tracker = DrawdownTracker()
    tracker.push(100.0)
    assert tracker.high_water_mark() == 100.0
    tracker.push(120.0)
    assert tracker.high_water_mark() == 120.0
    tracker.push(90.0)
    assert tracker.current_drawdown() == pytest.approx(30.0)
    assert tracker.max_drawdown() == pytest.approx(30.0)
    tracker.push(150.0)
    assert tracker.high_water_mark() == 150.0
    assert tracker.current_drawdown() == 0.0
    assert tracker.max_drawdown() == pytest.approx(30.0)


def test_drawdown_tracker_mean_and_variance():
    tracker = DrawdownTracker()
    for v in [2.0, 4.0, 4.0, 4.0]:
        tracker.push(v)
    assert tracker.mean() == pytest.approx(3.5)
    assert tracker.variance() == pytest.approx(1.0)
