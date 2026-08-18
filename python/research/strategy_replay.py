"""Calendar-spread strategy replay: the Python research counterpart to
`cpp::participant::strategy::CalendarSpreadStrategy` (AEGIS-024, AEGIS-080;
ADR-0025, ADR-0026).

This is explicitly a **research/reference implementation**, not a second
production strategy. It reproduces the approved decision semantics exactly:

* the z-score is scored against the *prior* window only, via
  :func:`research.signal_reference.rolling_zscore_reference` -- the same
  independent reference Batch 1 verified agrees with the compiled
  ``RollingZScore`` value for value;
* flat -> enter long/short spread when ``z`` crosses ``+-entry_threshold``;
* in a position -> exit (flatten) when ``|z| <= exit_threshold``, never add
  to the position or flip directly;
* every trade is the same fixed ``quantity_units`` on both legs.

This is the identical state machine
`cpp/participant/strategy/calendar_spread_strategy.cpp` implements; see that
file for the reference C++ implementation this mirrors.

**What is deliberately simplified, and why it is still valid for
AEGIS-024's purpose.** Unlike the C++ demo path
(`cpp/participant/app/participant_run.cpp`), this replay executes both legs
at the observation's own ``near_price``/``far_price`` directly -- it does not
synthesize a bid/ask spread. AEGIS-024 asks for roll-method *sensitivity*:
holding every other choice fixed and varying only the roll policy. Since this
same simplified fill convention is applied identically under every policy
compared, it cancels out of the reported differences; it is not, on its own,
evidence of real execution quality (ADR-0025).
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal
from enum import StrEnum

from research.calendar_spread import CalendarSpreadObservation
from research.signal_reference import rolling_zscore_reference

__all__ = [
    "PositionState",
    "ReplayConfig",
    "RoundTrip",
    "StrategyReplayResult",
    "replay_strategy",
]


class PositionState(StrEnum):
    FLAT = "flat"
    LONG_SPREAD = "long_spread"  # Long near, short far.
    SHORT_SPREAD = "short_spread"  # Short near, long far.


@dataclass(frozen=True, slots=True)
class ReplayConfig:
    zscore_window: int
    entry_threshold: float
    exit_threshold: float
    quantity_units: Decimal

    def __post_init__(self) -> None:
        if self.entry_threshold <= 0:
            raise ValueError(f"entry_threshold must be > 0, got {self.entry_threshold}")
        if not (0 <= self.exit_threshold < self.entry_threshold):
            raise ValueError(
                "exit_threshold must satisfy 0 <= exit_threshold < entry_threshold, got "
                f"exit_threshold={self.exit_threshold}, entry_threshold={self.entry_threshold}"
            )
        if self.quantity_units <= 0:
            raise ValueError(f"quantity_units must be > 0, got {self.quantity_units}")


@dataclass(frozen=True, slots=True)
class RoundTrip:
    """One complete open-to-close cycle. Always fully closes at the same
    fixed size it opened with -- this strategy never scales a position."""

    direction: PositionState  # LONG_SPREAD or SHORT_SPREAD; never FLAT.
    entry_as_of: date
    exit_as_of: date
    entry_spread: Decimal
    exit_spread: Decimal
    entry_z_score: float
    exit_z_score: float
    realized_pnl: Decimal


@dataclass(frozen=True, slots=True)
class StrategyReplayResult:
    signal_count: int  # entry_count + exit_count.
    entry_count: int
    exit_count: int
    round_trips: tuple[RoundTrip, ...]
    total_realized_pnl: Decimal
    final_position: PositionState
    open_position_entry_as_of: date | None
    open_position_entry_spread: Decimal | None
    # Mark-to-market of a position still open at the end of the observation
    # window, valued at the final observation's own near/far prices. Zero when
    # flat. Reported separately from realized P&L, never folded into it: a
    # comparison that comments only on realized P&L would call two runs
    # "identical" while one is holding a long spread and the other a short one
    # -- a real economic difference the realized figure cannot express.
    open_position_unrealized_pnl: Decimal = Decimal(0)
    total_pnl: Decimal = Decimal(0)  # realized + unrealized, for convenience.


def replay_strategy(
    observations: Sequence[CalendarSpreadObservation], config: ReplayConfig
) -> StrategyReplayResult:
    """Replays the approved entry/exit state machine over ``observations``,
    in the order supplied. Raises nothing on an empty or short sequence --
    a sequence with too few observations to ever cross the entry threshold
    honestly reports zero signals, not an error, since that is itself a
    real (if uninteresting) finding for the sensitivity comparison."""
    spreads = [float(o.spread) for o in observations]
    scores = list(rolling_zscore_reference(spreads, config.zscore_window))

    position = PositionState.FLAT
    entry_as_of: date | None = None
    entry_spread: Decimal | None = None
    entry_z: float | None = None
    entry_near_price: Decimal | None = None
    entry_far_price: Decimal | None = None

    entry_count = 0
    exit_count = 0
    round_trips: list[RoundTrip] = []

    for observation, z in zip(observations, scores, strict=True):
        abs_z = abs(z)

        if position is PositionState.FLAT:
            if z <= -config.entry_threshold:
                position = PositionState.LONG_SPREAD
            elif z >= config.entry_threshold:
                position = PositionState.SHORT_SPREAD
            else:
                continue
            entry_count += 1
            entry_as_of = observation.as_of
            entry_spread = observation.spread
            entry_z = z
            entry_near_price = observation.near_price
            entry_far_price = observation.far_price
            continue

        if abs_z <= config.exit_threshold:
            assert entry_as_of is not None
            assert entry_spread is not None
            assert entry_z is not None
            assert entry_near_price is not None
            assert entry_far_price is not None

            # Long spread: long near (bought at entry, sold at exit), short
            # far (sold at entry, bought at exit). Short spread: reversed.
            near_signed = config.quantity_units if position is PositionState.LONG_SPREAD else -config.quantity_units
            far_signed = -near_signed
            near_pnl = near_signed * (observation.near_price - entry_near_price)
            far_pnl = far_signed * (observation.far_price - entry_far_price)

            exit_count += 1
            round_trips.append(
                RoundTrip(
                    direction=position,
                    entry_as_of=entry_as_of,
                    exit_as_of=observation.as_of,
                    entry_spread=entry_spread,
                    exit_spread=observation.spread,
                    entry_z_score=entry_z,
                    exit_z_score=z,
                    realized_pnl=near_pnl + far_pnl,
                )
            )
            position = PositionState.FLAT
            entry_as_of = entry_spread = entry_z = entry_near_price = entry_far_price = None

    total_realized_pnl = sum((rt.realized_pnl for rt in round_trips), start=Decimal(0))

    # Mark any still-open position to the final observation's own prices.
    unrealized = Decimal(0)
    if position is not PositionState.FLAT and observations:
        assert entry_near_price is not None
        assert entry_far_price is not None
        final = observations[-1]
        near_signed = (
            config.quantity_units
            if position is PositionState.LONG_SPREAD
            else -config.quantity_units
        )
        far_signed = -near_signed
        unrealized = near_signed * (final.near_price - entry_near_price) + far_signed * (
            final.far_price - entry_far_price
        )

    return StrategyReplayResult(
        signal_count=entry_count + exit_count,
        entry_count=entry_count,
        exit_count=exit_count,
        round_trips=tuple(round_trips),
        total_realized_pnl=total_realized_pnl,
        final_position=position,
        open_position_entry_as_of=entry_as_of,
        open_position_entry_spread=entry_spread,
        open_position_unrealized_pnl=unrealized,
        total_pnl=total_realized_pnl + unrealized,
    )
