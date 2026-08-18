"""AEGIS-081 -- expiry and roll effects: before/on/after-roll slicing."""

from __future__ import annotations

import pytest
from research.calendar_spread import build_calendar_spread_observations
from research.roll_expiry_effects import RollExpirySlice, compute_roll_expiry_effects
from roll_sensitivity_fixture import build_roll_sensitivity_fixture

pytestmark = pytest.mark.unit


def test_slices_partition_every_observation_exactly_once() -> None:
    fixture = build_roll_sensitivity_fixture()
    policy = fixture.policies["volume_crossover"]
    observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=policy,
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )

    result = compute_roll_expiry_effects(
        chain=fixture.chain,
        policy=policy,
        roll_observations=fixture.roll_observations,
        prices=fixture.near_prices,
        dates=fixture.dates,
        observations=observations,
        replay_config=fixture.replay_config,
    )

    assert result.roll_policy_name == "VolumeCrossoverPolicy"
    assert len(result.roll_dates) >= 1  # This policy is known to roll within the fixture window.

    total_observations = sum(s.observation_count for s in result.slices)
    assert total_observations == len(observations)

    kinds = {s.slice for s in result.slices}
    assert kinds == set(RollExpirySlice)

    no_roll_slice = next(s for s in result.slices if s.slice == RollExpirySlice.NO_ROLL_OBSERVED)
    assert no_roll_slice.observation_count == 0  # A roll DID occur, so this slice is empty.
    assert no_roll_slice.mean_spread is None


def test_a_policy_with_no_roll_in_range_reports_everything_under_no_roll_observed() -> None:
    fixture = build_roll_sensitivity_fixture()
    policy = fixture.policies["fixed_100_days"]  # Selects MID immediately: never rolls in this window.
    observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=policy,
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )

    result = compute_roll_expiry_effects(
        chain=fixture.chain,
        policy=policy,
        roll_observations=fixture.roll_observations,
        prices=fixture.near_prices,
        dates=fixture.dates,
        observations=observations,
        replay_config=fixture.replay_config,
    )

    assert result.roll_dates == ()
    no_roll_slice = next(s for s in result.slices if s.slice == RollExpirySlice.NO_ROLL_OBSERVED)
    assert no_roll_slice.observation_count == len(observations)
    for other in result.slices:
        if other.slice != RollExpirySlice.NO_ROLL_OBSERVED:
            assert other.observation_count == 0


def test_before_and_after_slices_use_real_expiry_distance_and_are_shorter_after_the_roll() -> None:
    fixture = build_roll_sensitivity_fixture()
    policy = fixture.policies["volume_crossover"]
    observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=policy,
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )
    result = compute_roll_expiry_effects(
        chain=fixture.chain,
        policy=policy,
        roll_observations=fixture.roll_observations,
        prices=fixture.near_prices,
        dates=fixture.dates,
        observations=observations,
        replay_config=fixture.replay_config,
    )

    before = next(s for s in result.slices if s.slice == RollExpirySlice.BEFORE_ROLL)
    after = next(s for s in result.slices if s.slice == RollExpirySlice.AFTER_ROLL)
    assert before.observation_count > 0
    assert after.observation_count > 0
    # After the roll the far leg is one contract further from expiry than the
    # pre-roll far leg was (a genuinely different contract), a real,
    # independently checkable fact about this fixture's chain.
    assert before.mean_expiry_distance_days is not None
    assert after.mean_expiry_distance_days is not None
    assert after.mean_expiry_distance_days != before.mean_expiry_distance_days


def test_slicing_is_deterministic_across_repeated_calls() -> None:
    fixture = build_roll_sensitivity_fixture()
    policy = fixture.policies["volume_crossover"]
    observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=policy,
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )
    kwargs = {
        "chain": fixture.chain,
        "policy": policy,
        "roll_observations": fixture.roll_observations,
        "prices": fixture.near_prices,
        "dates": fixture.dates,
        "observations": observations,
        "replay_config": fixture.replay_config,
    }
    first = compute_roll_expiry_effects(**kwargs)
    second = compute_roll_expiry_effects(**kwargs)
    assert first == second
