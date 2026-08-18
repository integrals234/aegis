"""AEGIS-138/139-155 report rendering: deterministic serialization and a
genuine (not merely copied) portfolio-risk reconciliation."""

from __future__ import annotations

import json
from decimal import Decimal
from pathlib import Path

import pytest
from reports.portfolio_risk_report import PositionAccountingRecord, build_portfolio_risk_report, reconcile_exposure
from reports.rejection_report import build_rejection_report
from reports.validation_report import build_validation_report
from research.strategy_replay import ReplayConfig
from validation._fixtures import make_synthetic_spread_series
from validation.baselines import run_random_signal_baseline
from validation.rejection import evaluate_strategy_for_rejection
from validation.sensitivity import compute_transaction_cost_sensitivity
from validation.stability import compute_parameter_stability_surface

pytestmark = pytest.mark.unit


def test_validation_report_is_byte_identical_across_renders(repo_root: Path) -> None:
    obs = make_synthetic_spread_series("EQX", seed=1)
    stability = compute_parameter_stability_surface(
        obs, Decimal(1), zscore_windows=(20,), entry_thresholds=(2.0,), exit_thresholds=(0.5,)
    )
    kwargs = {
        "root": repo_root, "input_paths": ["data_samples/futures/bars/eqx.csv"], "dataset_id": "d",
        "roll_policy_name": "p", "strategy_config": {"entry_threshold": 2.0}, "stability": stability,
    }
    first = build_validation_report(**kwargs)  # type: ignore[arg-type]
    second = build_validation_report(**kwargs)  # type: ignore[arg-type]
    assert first == second


def test_validation_report_carries_the_data_honesty_disclosure(repo_root: Path) -> None:
    text = build_validation_report(
        repo_root, [], "d", "p", strategy_config={},
    )
    document = json.loads(text)
    assert "synthetic" in document["findings"]["data_disclosure"]
    assert "establishes live profitability" in document["findings"]["data_disclosure"]


def test_rejection_report_records_verdict_and_every_criterion(repo_root: Path) -> None:
    obs = make_synthetic_spread_series("EQX", seed=2)
    config = ReplayConfig(zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1))
    baseline = run_random_signal_baseline(obs, config, seed=3)
    cost = compute_transaction_cost_sensitivity(obs, config, cost_levels=(Decimal(0),))
    report = evaluate_strategy_for_rejection(baseline.result, cost_sensitivity=cost, min_round_trip_count=10_000)

    text = build_rejection_report(repo_root, [], "d", "p", "random_baseline", report, strategy_config={})
    document = json.loads(text)
    assert document["findings"]["verdict"] == "reject"
    assert len(document["findings"]["criteria"]) >= 2
    assert document["findings"]["triggering_criteria"]


def test_portfolio_risk_reconciliation_recomputes_not_copies() -> None:
    positions = [PositionAccountingRecord(instrument_id=1, quantity_units=10, mark_price_units=100, multiplier_units=5)]
    # gross = |10*100*5| = 5000; net = 5000.
    matching = reconcile_exposure(positions, reported_gross_exposure_units=5000, reported_net_exposure_units=5000)
    assert matching.reconciles

    mismatched = reconcile_exposure(positions, reported_gross_exposure_units=9999, reported_net_exposure_units=5000)
    assert not mismatched.reconciles
    assert not mismatched.gross_matches
    assert mismatched.net_matches


def test_portfolio_risk_report_flags_a_genuine_mismatch(repo_root: Path) -> None:
    positions = [PositionAccountingRecord(instrument_id=1, quantity_units=10, mark_price_units=100, multiplier_units=5)]
    text = build_portfolio_risk_report(
        repo_root, [], "d", strategy_config={}, positions=positions,
        reported_risk_analytics={"gross_exposure_units": 1, "net_exposure_units": 1},  # Deliberately wrong.
    )
    document = json.loads(text)
    assert document["findings"]["reconciliation"]["reconciles"] is False


def test_portfolio_risk_report_passes_through_margin_disclosure(repo_root: Path) -> None:
    text = build_portfolio_risk_report(
        repo_root, [], "d", strategy_config={}, positions=[],
        reported_risk_analytics={"gross_exposure_units": 0, "net_exposure_units": 0},
    )
    document = json.loads(text)
    assert "NOT SPAN" in document["findings"]["margin_model_disclosure"]
