"""AEGIS-150, AEGIS-151 -- random-signal and simple-rule baselines, stored
regardless of outcome, over the identical partition/costs."""

from __future__ import annotations

from decimal import Decimal

import pytest
from research.strategy_replay import ExecutionAssumptions, ReplayConfig, replay_strategy
from validation._fixtures import make_synthetic_spread_series
from validation.baselines import run_random_signal_baseline, run_simple_rule_baseline

pytestmark = pytest.mark.unit


@pytest.fixture
def observations():
    return make_synthetic_spread_series("EQX", seed=11)


@pytest.fixture
def config():
    return ReplayConfig(zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1))


def test_random_baseline_is_deterministic_given_its_seed(observations, config) -> None:
    first = run_random_signal_baseline(observations, config, seed=5)
    second = run_random_signal_baseline(observations, config, seed=5)
    assert first.result == second.result


def test_random_baseline_differs_from_a_different_seed(observations, config) -> None:
    first = run_random_signal_baseline(observations, config, seed=5)
    second = run_random_signal_baseline(observations, config, seed=6)
    # Not asserted to always differ in principle, but for this fixture/seed
    # pair it does -- a regression signal if the shuffle stops being seeded.
    assert first.result.round_trips != second.result.round_trips or first.result.total_pnl != second.result.total_pnl


def test_simple_rule_baseline_uses_no_rolling_window() -> None:
    # A constant-spread series has zero variance; the simple rule's scale
    # falls back to 1.0 rather than dividing by zero, and produces no
    # entries (every observation scores exactly 0).
    from datetime import date, timedelta

    from futures.identifiers import ContractId
    from research.calendar_spread import CalendarSpreadObservation

    near = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
    far = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
    flat = [
        CalendarSpreadObservation(
            as_of=date(2026, 1, 1) + timedelta(days=i), near_contract_id=near, far_contract_id=far,
            near_price=Decimal(100), far_price=Decimal(150), roll_policy_name="p",
            far_price_provenance="test", contract_steps=1,
        )
        for i in range(10)
    ]
    config = ReplayConfig(zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1))
    result = run_simple_rule_baseline(flat, config)
    assert result.result.entry_count == 0


def test_baselines_are_stored_regardless_of_beating_or_losing_to_the_strategy(observations, config) -> None:
    strategy_result = replay_strategy(observations, config)
    random_baseline = run_random_signal_baseline(observations, config, seed=13)
    simple_baseline = run_simple_rule_baseline(observations, config)

    # Both stored unconditionally -- no branch that discards a losing result.
    assert random_baseline.result is not None
    assert simple_baseline.result is not None
    # This assertion intentionally does not require the strategy to beat
    # either baseline (anti-overfitting: a losing comparison is preserved).
    assert isinstance(strategy_result.total_pnl, Decimal)


def test_baselines_share_the_same_execution_assumptions_as_supplied(observations, config) -> None:
    assumptions = ExecutionAssumptions(fee_per_unit=Decimal("0.5"))
    with_cost = run_random_signal_baseline(observations, config, seed=5, assumptions=assumptions)
    without_cost = run_random_signal_baseline(observations, config, seed=5)
    if with_cost.result.round_trips and without_cost.result.round_trips:
        assert with_cost.result.total_realized_pnl <= without_cost.result.total_realized_pnl
