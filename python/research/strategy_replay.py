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

# M5 addendum: ExecutionAssumptions (AEGIS-143..145)

:class:`ExecutionAssumptions`, added in M5, is a validation execution model,
not a claim of market realism (ADR-0029). Its default value
(``ExecutionAssumptions()``: zero delay, zero cost, ``FillAssumption.TOUCH``)
makes :func:`replay_strategy` produce byte-identical output to the pre-M5
signature -- every M4 caller is unaffected.

**Delay changes WHEN and WHETHER a trade fills, not just a number in a
report.** A signal detected at observation index ``i`` (using that
observation's own z-score -- the decision itself is instant) does not
execute at ``i``: the eligible execution index is
``i + decision_delay_days + execution_delay_days`` (plus one more under
``FillAssumption.CROSS_OR_NEXT``, see below), on this replay's own
deterministic daily observation grid. If that index falls outside the
supplied series, the documented rule is that the signal is DROPPED --
never filled, not filled at a fabricated future price. The fill, when it
happens, uses the EXECUTION index's own ``near_price``/``far_price``, not
the signal day's -- so ``RoundTrip.entry_as_of``/``exit_as_of`` report the
actual fill date, and two replays that differ only in delay produce
verifiably different timing and P&L on the same fixture.

**Two fill assumptions, genuinely different in effect, not label.**
``TOUCH`` fills at the earliest eligible index. ``CROSS_OR_NEXT`` requires
one additional bar of confirmation past that index -- a conservative model
of "the market must trade through the level, confirmed on the next
observation" -- which can push a fill past the end of the series where
``TOUCH`` would still have filled.
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
    "ExecutionAssumptions",
    "FillAssumption",
    "PositionState",
    "ReplayConfig",
    "RoundTrip",
    "StrategyReplayResult",
    "execution_index",
    "replay_strategy",
]


class PositionState(StrEnum):
    FLAT = "flat"
    LONG_SPREAD = "long_spread"  # Long near, short far.
    SHORT_SPREAD = "short_spread"  # Short near, long far.


class FillAssumption(StrEnum):
    TOUCH = "touch"  # Fills at the earliest eligible observation index.
    CROSS_OR_NEXT = "cross_or_next"  # Requires one additional bar of confirmation.


@dataclass(frozen=True, slots=True)
class ExecutionAssumptions:
    """A validation execution model (M5, ADR-0029) -- not observed fills,
    not a claim of market realism. See the module docstring for exactly how
    each field changes replay behaviour."""

    fee_per_unit: Decimal = Decimal(0)
    half_spread: Decimal = Decimal(0)
    slippage_per_unit: Decimal = Decimal(0)
    decision_delay_days: int = 0
    execution_delay_days: int = 0
    fill_assumption: FillAssumption = FillAssumption.TOUCH

    def __post_init__(self) -> None:
        if self.decision_delay_days < 0:
            raise ValueError(f"decision_delay_days must be >= 0, got {self.decision_delay_days}")
        if self.execution_delay_days < 0:
            raise ValueError(f"execution_delay_days must be >= 0, got {self.execution_delay_days}")
        for name, value in (
            ("fee_per_unit", self.fee_per_unit),
            ("half_spread", self.half_spread),
            ("slippage_per_unit", self.slippage_per_unit),
        ):
            if value < 0:
                raise ValueError(f"{name} must be >= 0, got {value}")

    @property
    def cost_per_unit_per_transaction(self) -> Decimal:
        """The adverse cost charged once per leg per transaction (open or
        close): fee plus the half-spread and slippage a real order would
        cross. A round trip incurs this four times -- open near, open far,
        close near, close far."""
        return self.fee_per_unit + self.half_spread + self.slippage_per_unit


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
    fixed size it opened with -- this strategy never scales a position.

    ``entry_as_of``/``exit_as_of`` are the actual FILL dates (the execution
    index's own date under ``ExecutionAssumptions``, identical to the signal
    date when assumptions are default) -- not necessarily the date the
    signal was detected on.
    """

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
    # M5: signals detected but never filled because ExecutionAssumptions'
    # delay pushed the eligible execution index past the end of the series
    # (documented deterministic rule, module docstring). Zero under default
    # assumptions.
    dropped_signal_count: int = 0


def execution_index(signal_index: int, series_length: int, assumptions: ExecutionAssumptions) -> int | None:
    target = signal_index + assumptions.decision_delay_days + assumptions.execution_delay_days
    if assumptions.fill_assumption is FillAssumption.CROSS_OR_NEXT:
        target += 1
    return target if target < series_length else None


def replay_strategy(
    observations: Sequence[CalendarSpreadObservation],
    config: ReplayConfig,
    assumptions: ExecutionAssumptions | None = None,
) -> StrategyReplayResult:
    """Replays the approved entry/exit state machine over ``observations``,
    in the order supplied. Raises nothing on an empty or short sequence --
    a sequence with too few observations to ever cross the entry threshold
    honestly reports zero signals, not an error, since that is itself a
    real (if uninteresting) finding for the sensitivity comparison.

    ``assumptions`` defaults to zero-delay/zero-cost/``TOUCH``, which is
    byte-identical to the pre-M5 behaviour (every signal fills at its own
    observation index, at that observation's own price, with no cost).
    """
    assumptions = assumptions if assumptions is not None else ExecutionAssumptions()
    spreads = [float(o.spread) for o in observations]
    scores = list(rolling_zscore_reference(spreads, config.zscore_window))
    series_length = len(observations)
    cost = assumptions.cost_per_unit_per_transaction * config.quantity_units

    position = PositionState.FLAT
    entry_as_of: date | None = None
    entry_spread: Decimal | None = None
    entry_z: float | None = None
    entry_near_price: Decimal | None = None
    entry_far_price: Decimal | None = None

    entry_count = 0
    exit_count = 0
    dropped_signal_count = 0
    round_trips: list[RoundTrip] = []

    for signal_index, (observation, z) in enumerate(zip(observations, scores, strict=True)):
        abs_z = abs(z)

        if position is PositionState.FLAT:
            if z <= -config.entry_threshold:
                candidate_position = PositionState.LONG_SPREAD
            elif z >= config.entry_threshold:
                candidate_position = PositionState.SHORT_SPREAD
            else:
                continue

            exec_index = execution_index(signal_index, series_length, assumptions)
            if exec_index is None:
                # The delayed timestamp has no eligible market observation
                # (module docstring's documented deterministic rule): the
                # signal is dropped, never fabricated a fill from data that
                # does not exist.
                dropped_signal_count += 1
                continue

            fill = observations[exec_index]
            position = candidate_position
            entry_count += 1
            entry_as_of = fill.as_of
            entry_spread = observation.spread  # The signal-day spread, for diagnostics.
            entry_z = z
            entry_near_price = fill.near_price
            entry_far_price = fill.far_price
            continue

        if abs_z <= config.exit_threshold:
            exec_index = execution_index(signal_index, series_length, assumptions)
            if exec_index is None:
                # The exit cannot fill either; the position stays open and is
                # reported via open_position_unrealized_pnl at the series end.
                dropped_signal_count += 1
                continue

            assert entry_as_of is not None
            assert entry_spread is not None
            assert entry_z is not None
            assert entry_near_price is not None
            assert entry_far_price is not None

            fill = observations[exec_index]
            # Long spread: long near (bought at entry, sold at exit), short
            # far (sold at entry, bought at exit). Short spread: reversed.
            near_signed = config.quantity_units if position is PositionState.LONG_SPREAD else -config.quantity_units
            far_signed = -near_signed
            near_pnl = near_signed * (fill.near_price - entry_near_price)
            far_pnl = far_signed * (fill.far_price - entry_far_price)
            # Four transactions per round trip (open near, open far, close
            # near, close far), each paying the adverse cost once.
            transaction_cost = cost * 4

            exit_count += 1
            round_trips.append(
                RoundTrip(
                    direction=position,
                    entry_as_of=entry_as_of,
                    exit_as_of=fill.as_of,
                    entry_spread=entry_spread,
                    exit_spread=observation.spread,
                    entry_z_score=entry_z,
                    exit_z_score=z,
                    realized_pnl=near_pnl + far_pnl - transaction_cost,
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
        dropped_signal_count=dropped_signal_count,
    )
