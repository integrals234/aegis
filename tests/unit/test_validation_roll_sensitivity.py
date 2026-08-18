"""AEGIS-154 -- roll-method sensitivity reuses M4's own comparison module
and reports differences honestly, including a genuine zero."""

from __future__ import annotations

from decimal import Decimal

import pytest
from research.roll_method_sensitivity import PolicyStrategyMetrics, RollMethodStrategySensitivityResult
from research.strategy_replay import PositionState
from validation.roll_sensitivity import summarize_roll_method_differences

pytestmark = pytest.mark.unit


def _metrics(name: str, total_pnl: Decimal) -> PolicyStrategyMetrics:
    return PolicyStrategyMetrics(
        policy_name=name, signal_count=0, entry_count=0, exit_count=0, round_trip_count=0,
        total_realized_pnl=total_pnl, open_position_unrealized_pnl=Decimal(0), total_pnl=total_pnl,
        final_position=PositionState.FLAT, contract_pairs=(),
    )


def test_reports_a_difference_for_every_distinct_policy_pair() -> None:
    result = RollMethodStrategySensitivityResult(
        price_path_comparisons=(),
        strategy_metrics_by_policy=(
            _metrics("PolicyA", Decimal(100)), _metrics("PolicyB", Decimal(80)), _metrics("PolicyC", Decimal(80)),
        ),
    )
    differences = summarize_roll_method_differences(result)
    assert len(differences) == 3  # C(3, 2).
    pairs = {(d.policy_a, d.policy_b) for d in differences}
    assert pairs == {("PolicyA", "PolicyB"), ("PolicyA", "PolicyC"), ("PolicyB", "PolicyC")}


def test_a_genuine_tie_is_reported_as_an_honest_zero_not_perturbed() -> None:
    result = RollMethodStrategySensitivityResult(
        price_path_comparisons=(),
        strategy_metrics_by_policy=(_metrics("PolicyA", Decimal(50)), _metrics("PolicyB", Decimal(50))),
    )
    differences = summarize_roll_method_differences(result)
    assert len(differences) == 1
    assert differences[0].total_pnl_difference == Decimal(0)


def test_single_policy_produces_no_pairs() -> None:
    result = RollMethodStrategySensitivityResult(
        price_path_comparisons=(), strategy_metrics_by_policy=(_metrics("Only", Decimal(1)),)
    )
    assert summarize_roll_method_differences(result) == ()
