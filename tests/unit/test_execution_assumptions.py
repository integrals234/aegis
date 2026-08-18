"""AEGIS-144, AEGIS-145 -- ExecutionAssumptions must genuinely alter
execution eligibility/timing and fill outcomes, not merely appear as a
report label."""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from research.calendar_spread import CalendarSpreadObservation
from research.strategy_replay import (
    ExecutionAssumptions,
    FillAssumption,
    PositionState,
    ReplayConfig,
    replay_strategy,
)

pytestmark = pytest.mark.unit

NEAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
FAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
BASE_DAY = date(2026, 1, 1)

# The same six-value sequence test_strategy_replay.py verifies against the
# compiled C++ strategy: entry signal at index 2, exit signal at index 5.
SPREADS = [Decimal("0.50"), Decimal("0.55"), Decimal("0.60"), Decimal("0.65"), Decimal("2.50"), Decimal("0.70")]


def _observations() -> list[CalendarSpreadObservation]:
    return [
        CalendarSpreadObservation(
            as_of=BASE_DAY + timedelta(days=i),
            near_contract_id=NEAR,
            far_contract_id=FAR,
            near_price=Decimal(100) + Decimal(i),  # Distinct per index, so a delayed fill uses a distinct price.
            far_price=Decimal(100) + Decimal(i) + spread,
            roll_policy_name="FixedDaysPolicy",
            far_price_provenance="test",
            contract_steps=1,
        )
        for i, spread in enumerate(SPREADS)
    ]


def _config() -> ReplayConfig:
    return ReplayConfig(zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1))


def test_zero_delay_matches_pre_m5_behaviour_exactly() -> None:
    result = replay_strategy(_observations(), _config())
    assert result.dropped_signal_count == 0
    assert len(result.round_trips) == 1
    rt = result.round_trips[0]
    assert rt.entry_as_of == BASE_DAY + timedelta(days=2)
    assert rt.exit_as_of == BASE_DAY + timedelta(days=5)


def test_decision_delay_shifts_the_fill_to_a_later_observation_with_a_different_price() -> None:
    zero_delay = replay_strategy(_observations(), _config())
    delayed = replay_strategy(
        _observations(), _config(), ExecutionAssumptions(decision_delay_days=1)
    )

    # Zero delay: entry at index 2 fills immediately, and the exit signal at
    # index 5 fills at index 5 too -- a completed round trip.
    assert len(zero_delay.round_trips) == 1
    zero_rt = zero_delay.round_trips[0]
    assert zero_rt.entry_as_of == BASE_DAY + timedelta(days=2)

    # decision_delay_days=1: entry fills one observation later (index 3, a
    # genuinely different date and price) -- but the exit signal at index 5
    # now targets index 6, past the end of the series, so it is dropped and
    # the position is left open instead of closing. Entry timing and the
    # eventual outcome both differ from the zero-delay run -- not just a
    # number printed in a report.
    assert delayed.round_trips == ()
    assert delayed.entry_count == 1
    assert delayed.open_position_entry_as_of == BASE_DAY + timedelta(days=3)
    assert delayed.dropped_signal_count == 1  # The exit attempt, dropped.
    assert delayed.total_pnl != zero_delay.total_pnl


def test_a_delay_pushing_past_the_end_of_the_series_drops_the_signal_deterministically() -> None:
    # Every entry signal in this fixture (indices 2, 3, 4 -- the strategy
    # stays flat and keeps re-evaluating each day a fill never lands, so all
    # three are attempted) targets an index at or beyond the 6-observation
    # series (indices 0..5) once decision_delay_days=4. The documented rule:
    # dropped, never filled at a fabricated price -- never a position opened.
    result = replay_strategy(_observations(), _config(), ExecutionAssumptions(decision_delay_days=4))
    assert result.entry_count == 0
    assert result.dropped_signal_count >= 1
    assert result.round_trips == ()
    assert result.final_position == PositionState.FLAT


def test_cross_or_next_can_drop_a_signal_that_touch_would_have_filled() -> None:
    # decision_delay_days=3 from signal index 2 -> target index 5, the LAST
    # valid index. TOUCH fills there and opens a position; CROSS_OR_NEXT's
    # extra confirmation bar needs index 6, which does not exist -- a
    # genuine difference in entry eligibility between the two fill
    # assumptions on the identical input (both still separately drop their
    # own delayed exit attempt, which this test does not need to assert).
    touch = replay_strategy(
        _observations(), _config(),
        ExecutionAssumptions(decision_delay_days=3, fill_assumption=FillAssumption.TOUCH),
    )
    cross_or_next = replay_strategy(
        _observations(), _config(),
        ExecutionAssumptions(decision_delay_days=3, fill_assumption=FillAssumption.CROSS_OR_NEXT),
    )

    assert touch.entry_count == 1
    assert cross_or_next.entry_count == 0
    assert cross_or_next.dropped_signal_count >= 1


def test_transaction_costs_reduce_realized_pnl_deterministically() -> None:
    baseline = replay_strategy(_observations(), _config())
    assumptions = ExecutionAssumptions(fee_per_unit=Decimal("0.01"), half_spread=Decimal("0.02"),
                                       slippage_per_unit=Decimal("0.005"))
    with_costs = replay_strategy(_observations(), _config(), assumptions)

    assert len(baseline.round_trips) == len(with_costs.round_trips) == 1
    # 4 transactions (open near, open far, close near, close far) * cost_per_unit * quantity_units(1).
    expected_cost = assumptions.cost_per_unit_per_transaction * 4
    assert baseline.round_trips[0].realized_pnl - with_costs.round_trips[0].realized_pnl == expected_cost


def test_execution_assumptions_rejects_negative_fields() -> None:
    with pytest.raises(ValueError, match="decision_delay_days"):
        ExecutionAssumptions(decision_delay_days=-1)
    with pytest.raises(ValueError, match="fee_per_unit"):
        ExecutionAssumptions(fee_per_unit=Decimal(-1))
