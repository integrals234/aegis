"""Random-signal and simple-rule baselines (AEGIS-150, AEGIS-151).

Both baselines run over the IDENTICAL data partition, cost assumptions and
execution assumptions as the strategy being validated -- comparability is
the whole point of a baseline. Both results are stored regardless of
whether they beat or lose to the strategy (never re-seeded/re-tuned until
one loses, per this milestone's anti-overfitting discipline).
"""

from __future__ import annotations

import random
from dataclasses import dataclass
from decimal import Decimal

from research.calendar_spread import CalendarSpreadObservation
from research.signal_reference import rolling_zscore_reference
from research.strategy_replay import (
    ExecutionAssumptions,
    PositionState,
    ReplayConfig,
    RoundTrip,
    StrategyReplayResult,
    execution_index,
)

__all__ = [
    "BaselineResult",
    "run_random_signal_baseline",
    "run_simple_rule_baseline",
]


@dataclass(frozen=True, slots=True)
class BaselineResult:
    name: str
    seed: int | None
    result: StrategyReplayResult


def run_random_signal_baseline(
    observations: tuple[CalendarSpreadObservation, ...],
    config: ReplayConfig,
    *,
    seed: int,
    assumptions: ExecutionAssumptions | None = None,
) -> BaselineResult:
    """AEGIS-150: the SAME entry/exit state machine, but driven by a
    deterministically shuffled copy of the real z-score series rather than
    the real one -- a signal with the same marginal distribution as the
    genuine one but no relationship to the actual spread path. Same
    partition/costs/execution assumptions as the strategy under test.
    """
    spreads = [float(o.spread) for o in observations]
    scores = list(rolling_zscore_reference(spreads, config.zscore_window))
    shuffled_scores = list(scores)
    random.Random(seed).shuffle(shuffled_scores)

    result = _replay_with_external_scores(observations, shuffled_scores, config, assumptions)
    return BaselineResult(name="random_shuffled_signal", seed=seed, result=result)


def run_simple_rule_baseline(
    observations: tuple[CalendarSpreadObservation, ...],
    config: ReplayConfig,
    assumptions: ExecutionAssumptions | None = None,
) -> BaselineResult:
    """AEGIS-151: a genuinely simpler rule -- a raw absolute spread
    threshold, with no rolling z-score at all. Enters when the spread
    itself (not standardized against any history) exceeds
    ``config.entry_threshold`` scaled to the spread's own units via the
    sample standard deviation of the full series (computed once, not
    rolling) -- simpler because it uses one global scale instead of an
    adaptive rolling window, while still comparable to the strategy's
    threshold semantics. Same partition/costs/execution assumptions.
    """
    spreads = [float(o.spread) for o in observations]
    mean = sum(spreads) / len(spreads) if spreads else 0.0
    variance = sum((s - mean) ** 2 for s in spreads) / len(spreads) if spreads else 0.0
    scale = variance**0.5 or 1.0  # A constant (never rolling) series has no informative threshold; avoid /0.
    raw_scores = [(s - mean) / scale for s in spreads]

    result = _replay_with_external_scores(observations, raw_scores, config, assumptions)
    return BaselineResult(name="simple_raw_spread_threshold", seed=None, result=result)


def _replay_with_external_scores(
    observations: tuple[CalendarSpreadObservation, ...],
    scores: list[float],
    config: ReplayConfig,
    assumptions: ExecutionAssumptions | None,
) -> StrategyReplayResult:
    """Runs the identical entry/exit/fill/cost machinery
    :func:`research.strategy_replay.replay_strategy` implements, but scored
    by an externally supplied signal instead of the real rolling z-score --
    reusing the entry/exit thresholds, fill-timing and cost model exactly,
    so only the SIGNAL differs between a baseline and the real strategy."""

    assumptions = assumptions if assumptions is not None else ExecutionAssumptions()
    series_length = len(observations)
    cost = assumptions.cost_per_unit_per_transaction * config.quantity_units

    position = PositionState.FLAT
    entry_as_of = entry_spread = entry_z = entry_near_price = entry_far_price = None
    entry_count = exit_count = dropped = 0
    round_trips: list[RoundTrip] = []

    for signal_index, (observation, z) in enumerate(zip(observations, scores, strict=True)):
        abs_z = abs(z)
        if position is PositionState.FLAT:
            if z <= -config.entry_threshold:
                candidate = PositionState.LONG_SPREAD
            elif z >= config.entry_threshold:
                candidate = PositionState.SHORT_SPREAD
            else:
                continue
            exec_index = execution_index(signal_index, series_length, assumptions)
            if exec_index is None:
                dropped += 1
                continue
            fill = observations[exec_index]
            position = candidate
            entry_count += 1
            entry_as_of, entry_spread, entry_z = fill.as_of, observation.spread, z
            entry_near_price, entry_far_price = fill.near_price, fill.far_price
            continue

        if abs_z <= config.exit_threshold:
            exec_index = execution_index(signal_index, series_length, assumptions)
            if exec_index is None:
                dropped += 1
                continue
            assert entry_as_of is not None
            assert entry_near_price is not None
            assert entry_far_price is not None
            assert entry_spread is not None
            assert entry_z is not None
            fill = observations[exec_index]
            near_signed = config.quantity_units if position is PositionState.LONG_SPREAD else -config.quantity_units
            far_signed = -near_signed
            near_pnl = near_signed * (fill.near_price - entry_near_price)
            far_pnl = far_signed * (fill.far_price - entry_far_price)
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
                    realized_pnl=near_pnl + far_pnl - cost * 4,
                )
            )
            position = PositionState.FLAT
            entry_as_of = entry_spread = entry_z = entry_near_price = entry_far_price = None

    total_realized = sum((rt.realized_pnl for rt in round_trips), start=Decimal(0))
    unrealized = Decimal(0)
    if position is not PositionState.FLAT and observations:
        assert entry_near_price is not None
        assert entry_far_price is not None
        final = observations[-1]
        near_signed = config.quantity_units if position is PositionState.LONG_SPREAD else -config.quantity_units
        far_signed = -near_signed
        unrealized = near_signed * (final.near_price - entry_near_price) + far_signed * (
            final.far_price - entry_far_price
        )

    return StrategyReplayResult(
        signal_count=entry_count + exit_count,
        entry_count=entry_count,
        exit_count=exit_count,
        round_trips=tuple(round_trips),
        total_realized_pnl=total_realized,
        final_position=position,
        open_position_entry_as_of=entry_as_of,
        open_position_entry_spread=entry_spread,
        open_position_unrealized_pnl=unrealized,
        total_pnl=total_realized + unrealized,
        dropped_signal_count=dropped,
    )
