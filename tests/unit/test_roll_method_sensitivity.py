"""AEGIS-024 -- the M4 residual: roll-method choice quantified against the
real calendar-spread strategy, not a separately-invented one.

`python/futures/roll_sensitivity.compare_roll_methods` (M2) is reused
unmodified for the price-path half; `research.strategy_replay.replay_strategy`
(the same decision semantics `cpp/participant/strategy` implements) is reused
unmodified for the strategy-level half.
"""

from __future__ import annotations

import pytest
from research.roll_method_sensitivity import (
    PolicyStrategyMetrics,
    compute_roll_method_strategy_sensitivity,
)
from research.strategy_replay import PositionState
from roll_sensitivity_fixture import build_roll_sensitivity_fixture

pytestmark = pytest.mark.unit


def test_two_policies_that_choose_different_roll_dates_are_flagged() -> None:
    fixture = build_roll_sensitivity_fixture()
    result = compute_roll_method_strategy_sensitivity(
        fixture.chain,
        fixture.policies,
        fixture.roll_observations,
        fixture.near_prices,
        fixture.dates,
        fixture.basis_rule,
        fixture.replay_config,
    )

    assert len(result.price_path_comparisons) == 1  # C(2, 2) pairs among 2 policies.
    comparison = result.price_path_comparisons[0]
    assert comparison.roll_dates_differ is True
    assert comparison.max_abs_price_deviation > 0
    assert comparison.mean_abs_price_deviation > 0
    # fixed_100_days selects MID immediately (no roll within the window);
    # volume_crossover starts at NEAR and confirms a crossover roll later.
    assert comparison.roll_dates_a == () or comparison.roll_dates_b == ()


def test_every_named_policy_gets_a_strategy_metrics_row() -> None:
    fixture = build_roll_sensitivity_fixture()
    result = compute_roll_method_strategy_sensitivity(
        fixture.chain,
        fixture.policies,
        fixture.roll_observations,
        fixture.near_prices,
        fixture.dates,
        fixture.basis_rule,
        fixture.replay_config,
    )
    policy_names = {m.policy_name for m in result.strategy_metrics_by_policy}
    assert policy_names == set(fixture.policies)
    for metrics in result.strategy_metrics_by_policy:
        assert metrics.signal_count == metrics.entry_count + metrics.exit_count
        assert metrics.round_trip_count == metrics.exit_count
        assert metrics.final_position in PositionState


def test_sensitivity_computation_is_deterministic() -> None:
    fixture = build_roll_sensitivity_fixture()
    args = (
        fixture.chain,
        fixture.policies,
        fixture.roll_observations,
        fixture.near_prices,
        fixture.dates,
        fixture.basis_rule,
        fixture.replay_config,
    )
    first = compute_roll_method_strategy_sensitivity(*args)
    second = compute_roll_method_strategy_sensitivity(*args)
    assert first == second


def test_strategy_metrics_reflect_the_real_replayed_state_machine_not_invented_numbers() -> None:
    """Cross-check: rebuild one policy's observations independently and
    replay them directly, and confirm the sensitivity result's row for that
    policy matches -- the sensitivity module must not compute its own
    parallel notion of "signal count"."""
    from research.calendar_spread import build_calendar_spread_observations
    from research.strategy_replay import replay_strategy

    fixture = build_roll_sensitivity_fixture()
    result = compute_roll_method_strategy_sensitivity(
        fixture.chain,
        fixture.policies,
        fixture.roll_observations,
        fixture.near_prices,
        fixture.dates,
        fixture.basis_rule,
        fixture.replay_config,
    )

    policy_name = "volume_crossover"
    observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=fixture.policies[policy_name],
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )
    independent = replay_strategy(observations, fixture.replay_config)
    row = next(m for m in result.strategy_metrics_by_policy if m.policy_name == policy_name)

    assert row.signal_count == independent.signal_count
    assert row.entry_count == independent.entry_count
    assert row.exit_count == independent.exit_count
    assert row.total_realized_pnl == independent.total_realized_pnl
    assert row.final_position == independent.final_position


def test_does_not_claim_any_policy_is_better_only_reports_differences() -> None:
    """No production/report field here is named or shaped as a
    recommendation -- AEGIS-024 asks for sensitivity, not optimization."""
    import dataclasses

    metric_field_names = {field.name for field in dataclasses.fields(PolicyStrategyMetrics)}
    forbidden = {"best_policy", "recommended", "ranking", "winner", "optimal_policy"}
    assert not (metric_field_names & forbidden)


def test_roll_choice_produces_a_genuinely_different_strategy_outcome() -> None:
    """AEGIS-024's frozen acceptance asks for an experiment report that
    QUANTIFIES strategy differences caused by roll choices. This asserts the
    experiment actually resolves such a difference rather than reporting a
    degenerate zero: the two policies select different contract pairs, take
    opposite positions, and end with different mark-to-market P&L."""
    fixture = build_roll_sensitivity_fixture()
    result = compute_roll_method_strategy_sensitivity(
        fixture.chain,
        fixture.policies,
        fixture.roll_observations,
        fixture.near_prices,
        fixture.dates,
        fixture.basis_rule,
        fixture.replay_config,
    )

    by_name = {m.policy_name: m for m in result.strategy_metrics_by_policy}
    volume = by_name["volume_crossover"]
    fixed = by_name["fixed_100_days"]

    # Different contract pairs traded -- the direct consequence of roll choice.
    assert volume.contract_pairs != fixed.contract_pairs
    # Opposite positions, and a genuinely different economic outcome.
    assert volume.final_position != fixed.final_position
    assert volume.total_pnl != fixed.total_pnl


def test_every_policys_far_leg_is_observed_so_the_comparison_is_controlled() -> None:
    """If one policy's far leg were observed and another's constructed, the
    comparison would be confounded by provenance rather than controlled by
    roll choice alone."""
    from research.calendar_spread import build_calendar_spread_observations

    fixture = build_roll_sensitivity_fixture()
    for name, policy in fixture.policies.items():
        observations = build_calendar_spread_observations(
            chain=fixture.chain,
            policy=policy,
            roll_observations=fixture.roll_observations,
            near_prices=fixture.near_prices,
            as_of_dates=fixture.dates,
            basis_rule=fixture.basis_rule,
        )
        assert all(o.far_price_observed for o in observations), name
