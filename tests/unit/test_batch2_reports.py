"""AEGIS-079/081/024 report content, built on Batch 1's shared report
foundation (`python/reports/report_model.py`). One test module covering all
three thin report builders -- they are not three unrelated frameworks."""

from __future__ import annotations

from pathlib import Path

import pytest
from reports.roll_expiry_report import build_roll_expiry_report
from reports.roll_sensitivity_report import build_roll_sensitivity_report
from reports.stationarity_report import build_stationarity_report
from research.calendar_spread import build_calendar_spread_observations
from research.roll_expiry_effects import compute_roll_expiry_effects
from research.roll_method_sensitivity import compute_roll_method_strategy_sensitivity
from research.stationarity import test_spread_stationarity as run_stationarity_test
from roll_sensitivity_fixture import build_roll_sensitivity_fixture

pytestmark = pytest.mark.unit

INPUT_PATH = "data_samples/futures/bars/eqx.csv"  # A small, real, already-committed input.


@pytest.fixture(scope="module")
def fixture():
    return build_roll_sensitivity_fixture()


def test_stationarity_report_is_byte_identical_across_renders(repo_root: Path, fixture) -> None:
    observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=fixture.policies["volume_crossover"],
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )
    result = run_stationarity_test(observations)

    first = build_stationarity_report(repo_root, [INPUT_PATH], "CSX-synthetic", "VolumeCrossoverPolicy", result)
    second = build_stationarity_report(repo_root, [INPUT_PATH], "CSX-synthetic", "VolumeCrossoverPolicy", result)
    assert first == second
    assert '"classification":"' + result.classification.value + '"' in first
    assert "not a claim" in first


def test_roll_expiry_report_is_byte_identical_across_renders(repo_root: Path, fixture) -> None:
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

    first = build_roll_expiry_report(repo_root, [INPUT_PATH], "CSX-synthetic", result)
    second = build_roll_expiry_report(repo_root, [INPUT_PATH], "CSX-synthetic", result)
    assert first == second
    assert "before_roll" in first
    assert "after_roll" in first


def test_roll_sensitivity_report_is_byte_identical_across_renders(repo_root: Path, fixture) -> None:
    result = compute_roll_method_strategy_sensitivity(
        fixture.chain,
        fixture.policies,
        fixture.roll_observations,
        fixture.near_prices,
        fixture.dates,
        fixture.basis_rule,
        fixture.replay_config,
    )

    first = build_roll_sensitivity_report(repo_root, [INPUT_PATH], "CSX-synthetic", result)
    second = build_roll_sensitivity_report(repo_root, [INPUT_PATH], "CSX-synthetic", result)
    assert first == second
    assert "does not claim any policy is universally better" in first
    assert "volume_crossover" in first
    assert "fixed_100_days" in first


def test_reports_disclose_the_constructed_data_convention(repo_root: Path, fixture) -> None:
    observations = build_calendar_spread_observations(
        chain=fixture.chain,
        policy=fixture.policies["volume_crossover"],
        roll_observations=fixture.roll_observations,
        near_prices=fixture.near_prices,
        as_of_dates=fixture.dates,
        basis_rule=fixture.basis_rule,
    )
    result = run_stationarity_test(observations)
    rendered = build_stationarity_report(repo_root, [INPUT_PATH], "CSX-synthetic", "VolumeCrossoverPolicy", result)
    assert "not observed" in rendered
    assert "ADR-0025" in rendered
