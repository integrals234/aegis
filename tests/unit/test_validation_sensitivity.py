"""AEGIS-143, AEGIS-144, AEGIS-145 -- cost, latency and slippage/fill
sensitivity sweeps report every level, and latency/fill genuinely change
outcomes (the underlying mechanism is proven directly in
test_execution_assumptions.py; here we prove the sweep/report layer)."""

from __future__ import annotations

from decimal import Decimal

import pytest
from research.strategy_replay import ReplayConfig
from validation._fixtures import make_synthetic_spread_series
from validation.sensitivity import (
    compute_latency_sensitivity,
    compute_slippage_and_fill_sensitivity,
    compute_transaction_cost_sensitivity,
)

pytestmark = pytest.mark.unit


@pytest.fixture
def observations():
    return make_synthetic_spread_series("EQX", seed=3)


@pytest.fixture
def config():
    return ReplayConfig(zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1))


def test_cost_sweep_reports_every_level_in_order(observations, config) -> None:
    levels = (Decimal(0), Decimal("0.5"), Decimal(2), Decimal(10))
    result = compute_transaction_cost_sensitivity(observations, config, cost_levels=levels)
    assert len(result.points) == 4
    assert [p.fee_per_unit for p in result.points] == list(levels)


def test_cost_sweep_pnl_is_non_increasing_as_cost_rises(observations, config) -> None:
    levels = (Decimal(0), Decimal("0.5"), Decimal(2), Decimal(10))
    result = compute_transaction_cost_sensitivity(observations, config, cost_levels=levels)
    pnls = [p.total_pnl for p in result.points]
    assert pnls == sorted(pnls, reverse=True)


def test_break_even_index_is_none_when_costs_never_cross_it(observations, config) -> None:
    result = compute_transaction_cost_sensitivity(observations, config, cost_levels=(Decimal(0),))
    if result.points[0].total_pnl > 0:
        assert result.break_even_index is None


def test_latency_sweep_covers_the_full_delay_cross_product(observations, config) -> None:
    result = compute_latency_sensitivity(
        observations, config, decision_delays=(0, 1, 2), execution_delays=(0, 1)
    )
    assert len(result.points) == 6
    assert {(p.decision_delay_days, p.execution_delay_days) for p in result.points} == {
        (d, e) for d in (0, 1, 2) for e in (0, 1)
    }


def test_zero_and_nonzero_latency_produce_a_recorded_difference_in_this_sweep(observations, config) -> None:
    # The mechanism proof (delay changes fill timing/eligibility) lives in
    # test_execution_assumptions.py; this proves the sweep surfaces it.
    result = compute_latency_sensitivity(observations, config, decision_delays=(0, 40), execution_delays=(0,))
    zero_delay = next(p for p in result.points if p.decision_delay_days == 0)
    large_delay = next(p for p in result.points if p.decision_delay_days == 40)
    # A 40-day delay on a 120-day series must drop or shift enough signals
    # to differ from zero delay on at least one reported field.
    assert (zero_delay.total_pnl, zero_delay.round_trip_count) != (
        large_delay.total_pnl, large_delay.round_trip_count,
    ) or zero_delay.dropped_signal_count != large_delay.dropped_signal_count


def test_fill_sensitivity_sweeps_both_slippage_and_fill_assumption(observations, config) -> None:
    result = compute_slippage_and_fill_sensitivity(observations, config, slippage_levels=(Decimal(0), Decimal("0.1")))
    assert len(result.points) == 4  # 2 slippage levels x 2 fill assumptions.
    assumptions_seen = {p.fill_assumption for p in result.points}
    assert len(assumptions_seen) == 2
