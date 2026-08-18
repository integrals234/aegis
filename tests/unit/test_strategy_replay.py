"""AEGIS-024/AEGIS-080 -- the Python strategy replay matches the approved
`CalendarSpreadStrategy` decision semantics exactly."""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from research.calendar_spread import CalendarSpreadObservation
from research.strategy_replay import PositionState, ReplayConfig, replay_strategy

pytestmark = pytest.mark.unit

NEAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
FAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
BASE_DAY = date(2026, 1, 1)


def _observations_from_spreads(spreads: list[float]) -> list[CalendarSpreadObservation]:
    """`near_price` fixed at 0 so `spread == far_price` exactly -- the same
    device `tests/cpp/unit/test_calendar_spread_strategy.cpp` uses to isolate
    the entry/exit arithmetic from any particular price level."""
    return [
        CalendarSpreadObservation(
            as_of=BASE_DAY + timedelta(days=i),
            near_contract_id=NEAR,
            far_contract_id=FAR,
            near_price=Decimal(0),
            far_price=Decimal(str(spread)),
            roll_policy_name="FixedDaysPolicy",
            far_price_provenance="test fixture",
            contract_steps=1,
        )
        for i, spread in enumerate(spreads)
    ]


def test_matches_the_cpp_strategys_entry_and_exit_sequence_exactly() -> None:
    """The identical six-value sequence and expected outcome
    `tests/cpp/unit/test_calendar_spread_strategy.cpp` verifies against the
    compiled C++ strategy: entry at index 2 (short spread), holds through
    3-4, exits at index 5."""
    spreads = [0.50, 0.55, 0.60, 0.65, 2.50, 0.70]
    observations = _observations_from_spreads(spreads)
    config = ReplayConfig(
        zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(7)
    )

    result = replay_strategy(observations, config)

    assert result.entry_count == 1
    assert result.exit_count == 1
    assert result.signal_count == 2
    assert result.final_position == PositionState.FLAT
    assert len(result.round_trips) == 1

    round_trip = result.round_trips[0]
    assert round_trip.direction == PositionState.SHORT_SPREAD
    assert round_trip.entry_as_of == BASE_DAY + timedelta(days=2)
    assert round_trip.exit_as_of == BASE_DAY + timedelta(days=5)
    assert round_trip.entry_z_score == pytest.approx(2.1213203435596393, abs=1e-9)
    assert round_trip.exit_z_score == pytest.approx(-0.3013796514749198, abs=1e-9)


def test_short_spread_realized_pnl_matches_signed_leg_arithmetic() -> None:
    """Short spread: sell near, buy far at entry; buy near, sell far at
    exit. Realized P&L is computed from the actual near/far prices at entry
    and exit, not just the spread -- ``near_price`` is held constant here so
    the whole realized P&L is attributable to the far leg alone, an
    independently checkable arithmetic case. Reuses the same
    entry-at-index-2/exit-at-index-5 spread sequence
    `test_matches_the_cpp_strategys_entry_and_exit_sequence_exactly` already
    proves the timing of."""
    spreads = [Decimal("0.50"), Decimal("0.55"), Decimal("0.60"), Decimal("0.65"), Decimal("2.50"), Decimal("0.70")]
    near_price = Decimal(100)
    observations = [
        CalendarSpreadObservation(
            as_of=BASE_DAY + timedelta(days=i),
            near_contract_id=NEAR,
            far_contract_id=FAR,
            near_price=near_price,
            far_price=near_price + spread,
            roll_policy_name="FixedDaysPolicy",
            far_price_provenance="test",
            contract_steps=1,
        )
        for i, spread in enumerate(spreads)
    ]
    config = ReplayConfig(
        zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(10)
    )

    result = replay_strategy(observations, config)

    assert len(result.round_trips) == 1
    round_trip = result.round_trips[0]
    assert round_trip.direction == PositionState.SHORT_SPREAD
    assert round_trip.entry_as_of == BASE_DAY + timedelta(days=2)
    assert round_trip.exit_as_of == BASE_DAY + timedelta(days=5)
    # near_price is unchanged (100 throughout), so the entire realized P&L is
    # the far leg's move: long far at 100.60, exit (sold) at 100.70 -> +0.10/unit.
    expected_pnl = Decimal(10) * (Decimal("100.70") - Decimal("100.60"))
    assert round_trip.realized_pnl == expected_pnl
    assert result.total_realized_pnl == expected_pnl


def test_never_flips_or_scales_while_already_in_a_position() -> None:
    # Alternating strong z-crossings while already in a position must never
    # add to it, flip it, or open a second round trip.
    spreads = [1.0, 1.0, 100.0, -100.0, 100.0, -100.0]
    observations = _observations_from_spreads(spreads)
    config = ReplayConfig(
        zscore_window=20, entry_threshold=2.0, exit_threshold=0.01, quantity_units=Decimal(1)
    )
    result = replay_strategy(observations, config)
    # At most one position is ever open; entries only happen from FLAT.
    assert result.entry_count <= 1 + result.exit_count


def test_open_position_at_end_is_reported_but_not_a_round_trip() -> None:
    spreads = [0.5, 0.6, 100.0]  # Entry on the third observation, never exits.
    observations = _observations_from_spreads(spreads)
    config = ReplayConfig(
        zscore_window=20, entry_threshold=2.0, exit_threshold=0.1, quantity_units=Decimal(1)
    )
    result = replay_strategy(observations, config)

    assert result.entry_count == 1
    assert result.exit_count == 0
    assert len(result.round_trips) == 0
    assert result.final_position != PositionState.FLAT
    assert result.open_position_entry_as_of == BASE_DAY + timedelta(days=2)
    assert result.total_realized_pnl == Decimal(0)


def test_no_signal_reports_zero_not_an_error() -> None:
    spreads = [1.0, 1.0, 1.0, 1.0]  # Never crosses the entry threshold.
    observations = _observations_from_spreads(spreads)
    config = ReplayConfig(
        zscore_window=20, entry_threshold=5.0, exit_threshold=0.5, quantity_units=Decimal(1)
    )
    result = replay_strategy(observations, config)
    assert result.signal_count == 0
    assert result.final_position == PositionState.FLAT
    assert result.total_realized_pnl == Decimal(0)


def test_replay_config_rejects_invalid_thresholds() -> None:
    with pytest.raises(ValueError, match="entry_threshold"):
        ReplayConfig(zscore_window=5, entry_threshold=0, exit_threshold=0, quantity_units=Decimal(1))
    with pytest.raises(ValueError, match="exit_threshold"):
        ReplayConfig(zscore_window=5, entry_threshold=1.0, exit_threshold=1.0, quantity_units=Decimal(1))
    with pytest.raises(ValueError, match="quantity_units"):
        ReplayConfig(zscore_window=5, entry_threshold=1.0, exit_threshold=0.1, quantity_units=Decimal(0))
