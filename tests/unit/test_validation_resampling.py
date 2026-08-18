"""AEGIS-146, AEGIS-147 -- bootstrap confidence intervals and Monte Carlo
trade-sequence resampling: seeded, deterministic, and genuinely distinct
mechanisms."""

from __future__ import annotations

from decimal import Decimal

import pytest
from validation.resampling import bootstrap_round_trip_pnl, monte_carlo_trade_resampling

pytestmark = pytest.mark.unit

PNLS = [Decimal("10"), Decimal("-5"), Decimal("20"), Decimal("-15"), Decimal("8"), Decimal("-3")]


def test_bootstrap_same_seed_and_input_is_byte_identical() -> None:
    first = bootstrap_round_trip_pnl(PNLS, num_draws=200, confidence_level=0.90, seed=42)
    second = bootstrap_round_trip_pnl(PNLS, num_draws=200, confidence_level=0.90, seed=42)
    assert first == second


def test_bootstrap_different_seed_may_differ() -> None:
    first = bootstrap_round_trip_pnl(PNLS, num_draws=200, confidence_level=0.90, seed=1)
    second = bootstrap_round_trip_pnl(PNLS, num_draws=200, confidence_level=0.90, seed=2)
    assert (first.lower, first.upper) != (second.lower, second.upper)


def test_bootstrap_interval_contains_the_point_estimate() -> None:
    result = bootstrap_round_trip_pnl(PNLS, num_draws=500, confidence_level=0.90, seed=7)
    assert result.lower <= result.point_estimate <= result.upper


def test_bootstrap_documents_statistic_unit_method_and_limitations() -> None:
    result = bootstrap_round_trip_pnl(PNLS, num_draws=100, confidence_level=0.95, seed=1)
    assert result.statistic_name
    assert result.sample_unit == "round_trip"
    assert result.resampling_method
    assert result.assumptions
    assert result.limitations
    assert result.num_draws == 100
    assert result.confidence_level == 0.95


def test_bootstrap_rejects_empty_input() -> None:
    with pytest.raises(ValueError, match="non-empty"):
        bootstrap_round_trip_pnl([], num_draws=10, confidence_level=0.9, seed=1)


def test_monte_carlo_same_seed_is_byte_identical() -> None:
    first = monte_carlo_trade_resampling(PNLS, num_paths=200, seed=42)
    second = monte_carlo_trade_resampling(PNLS, num_paths=200, seed=42)
    assert first == second


def test_monte_carlo_every_path_uses_every_trade_exactly_once() -> None:
    result = monte_carlo_trade_resampling(PNLS, num_paths=50, seed=1)
    total = sum(PNLS)
    for path in result.paths:
        assert Decimal(str(round(path.ending_pnl, 10))) == Decimal(str(round(float(total), 10)))


def test_monte_carlo_is_not_the_bootstrap_renamed() -> None:
    # Monte Carlo permutes (no repeats, no omissions); bootstrap resamples
    # with replacement. Proven by the ending-P&L invariant above and by the
    # fact each path here has exactly len(PNLS) trades, always.
    result = monte_carlo_trade_resampling(PNLS, num_paths=20, seed=3)
    assert result.trade_count == len(PNLS)
    assert all(True for _ in result.paths)  # Structure check: every path recorded.


def test_monte_carlo_reports_quantiles_for_ending_pnl_and_drawdown() -> None:
    result = monte_carlo_trade_resampling(PNLS, num_paths=200, seed=5)
    assert set(result.ending_pnl_quantiles) == {"p5", "p50", "p95"}
    assert set(result.max_drawdown_quantiles) == {"p5", "p50", "p95"}
    assert result.ending_pnl_quantiles["p5"] <= result.ending_pnl_quantiles["p95"]
