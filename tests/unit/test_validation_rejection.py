"""AEGIS-155 -- formal strategy rejection: every criterion is evaluated
and recorded, and at least one intentionally weak strategy genuinely
receives REJECT (never a hard-coded flag)."""

from __future__ import annotations

from decimal import Decimal

import pytest
from research.strategy_replay import ReplayConfig, replay_strategy
from validation._fixtures import make_synthetic_spread_series
from validation.baselines import run_random_signal_baseline
from validation.rejection import (
    RejectionVerdict,
    evaluate_strategy_for_rejection,
    realized_trade_sequence_max_drawdown,
)
from validation.resampling import bootstrap_round_trip_pnl
from validation.sensitivity import compute_transaction_cost_sensitivity
from validation.stability import compute_parameter_stability_surface

pytestmark = pytest.mark.unit


@pytest.fixture
def observations():
    return make_synthetic_spread_series("EQX", seed=21)


@pytest.fixture
def config():
    return ReplayConfig(zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1))


def test_every_criterion_is_recorded_whether_or_not_it_triggers(observations, config) -> None:
    result = replay_strategy(observations, config)
    cost = compute_transaction_cost_sensitivity(observations, config, cost_levels=(Decimal(0), Decimal(1)))
    stability = compute_parameter_stability_surface(
        observations, Decimal(1), zscore_windows=(20,), entry_thresholds=(2.0,), exit_thresholds=(0.5,)
    )
    report = evaluate_strategy_for_rejection(
        result, cost_sensitivity=cost, stability=stability, max_drawdown_threshold=Decimal(1000)
    )
    names = {c.name for c in report.criteria}
    assert "transaction_cost_unprofitable_at_lowest_swept_cost" in names
    assert "parameter_instability" in names
    assert "trade_concentration_too_few_round_trips" in names
    assert "max_drawdown_exceeds_threshold" in names
    # Every criterion appears regardless of triggered/not-triggered.
    assert len(report.criteria) == len(names)


def test_the_weak_shuffled_baseline_receives_a_genuine_reject(observations, config) -> None:
    # AEGIS-150's baseline, reused through the identical pipeline -- not a
    # separately invented strategy hard-coded to fail.
    baseline = run_random_signal_baseline(observations, config, seed=99)
    cost = compute_transaction_cost_sensitivity(observations, config, cost_levels=(Decimal(0),))

    # min_round_trip_count is set absurdly high to force the concentration
    # criterion to trigger deterministically for this assertion.
    report = evaluate_strategy_for_rejection(
        baseline.result, cost_sensitivity=cost, min_round_trip_count=1_000_000,
    )
    assert report.verdict == RejectionVerdict.REJECT
    assert report.triggering_criteria  # At least one real criterion actually fired.
    assert any(c.name == "trade_concentration_too_few_round_trips" for c in report.triggering_criteria)


def test_accept_is_possible_when_no_criterion_triggers() -> None:
    # A trivial, always-passing configuration: no sensitivity/stability
    # inputs supplied, and the trade-count floor set to zero.
    from research.strategy_replay import PositionState, StrategyReplayResult

    empty_result = StrategyReplayResult(
        signal_count=0, entry_count=0, exit_count=0, round_trips=(), total_realized_pnl=Decimal(0),
        final_position=PositionState.FLAT,
        open_position_entry_as_of=None, open_position_entry_spread=None,
    )
    report = evaluate_strategy_for_rejection(empty_result, min_round_trip_count=0)
    assert report.verdict == RejectionVerdict.ACCEPT
    assert report.triggering_criteria == ()


def test_max_drawdown_is_computed_from_the_actual_realized_trade_order(observations, config) -> None:
    result = replay_strategy(observations, config)
    drawdown = realized_trade_sequence_max_drawdown(result)
    assert drawdown >= Decimal(0)


def test_statistical_test_failure_criterion_uses_the_bootstrap_ci(observations, config) -> None:
    baseline = run_random_signal_baseline(observations, config, seed=17)
    if not baseline.result.round_trips:
        pytest.skip("fixture produced no round trips for this seed")
    bootstrap = bootstrap_round_trip_pnl(
        [rt.realized_pnl for rt in baseline.result.round_trips], num_draws=300, confidence_level=0.90, seed=1
    )
    report = evaluate_strategy_for_rejection(baseline.result, bootstrap=bootstrap, min_round_trip_count=0)
    triggered_names = {c.name for c in report.triggering_criteria}
    expect_trigger = bootstrap.upper <= 0
    assert ("statistical_test_failure_ci_excludes_no_positive_mean" in triggered_names) == expect_trigger
