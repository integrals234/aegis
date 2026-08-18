"""Expiry and roll effects on calendar-spread behaviour (AEGIS-081).

Slices a :class:`~research.calendar_spread.CalendarSpreadObservation`
sequence into *before*, *on*, and *after* the roll dates a real
:func:`futures.roll_audit.build_roll_audit` detects for the same chain and
policy, then computes deterministic per-slice metrics. No roll or expiry
logic is re-derived here -- the roll dates themselves are `build_roll_audit`'s
own output (AEGIS-023, M2), unmodified.

This is the M4 requirement's specific roll/expiry analysis: how calendar-
spread behaviour (spread level, term-structure state, and this milestone's
own strategy signal activity) differs around a roll. It is not the general
cross-strategy attribution framework reserved for M6.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal
from enum import StrEnum

from futures.chain import ContractChain
from futures.roll.policy import RollObservation, RollPolicy
from futures.roll_audit import build_roll_audit
from futures.series import PriceObservation

from research.calendar_spread import CalendarSpreadObservation
from research.strategy_replay import ReplayConfig, replay_strategy
from research.term_structure import compute_term_structure_features

__all__ = [
    "RollExpiryEffectsResult",
    "RollExpirySlice",
    "RollExpirySliceMetrics",
    "compute_roll_expiry_effects",
]


class RollExpirySlice(StrEnum):
    BEFORE_ROLL = "before_roll"
    ON_ROLL = "on_roll"
    AFTER_ROLL = "after_roll"
    NO_ROLL_OBSERVED = "no_roll_observed"  # No roll fell within the supplied date range.


@dataclass(frozen=True, slots=True)
class RollExpirySliceMetrics:
    slice: RollExpirySlice
    observation_count: int
    mean_spread: Decimal | None  # None when the slice has no observations.
    mean_expiry_distance_days: float | None
    entry_count: int
    exit_count: int


@dataclass(frozen=True, slots=True)
class RollExpiryEffectsResult:
    roll_policy_name: str
    roll_dates: tuple[date, ...]
    slices: tuple[RollExpirySliceMetrics, ...]


def _classify(as_of: date, roll_dates: Sequence[date]) -> RollExpirySlice:
    if not roll_dates:
        return RollExpirySlice.NO_ROLL_OBSERVED
    if as_of in roll_dates:
        return RollExpirySlice.ON_ROLL
    # Classified relative to the nearest roll date -- before the earliest
    # roll is BEFORE_ROLL, after the latest is AFTER_ROLL; a date strictly
    # between two rolls counts as AFTER the earlier one, which is the
    # simplest well-defined convention for the single- or few-roll windows
    # this module is used against.
    if as_of < min(roll_dates):
        return RollExpirySlice.BEFORE_ROLL
    return RollExpirySlice.AFTER_ROLL


def compute_roll_expiry_effects(
    chain: ContractChain,
    policy: RollPolicy,
    roll_observations: Sequence[RollObservation],
    prices: Sequence[PriceObservation],
    dates: Sequence[date],
    observations: Sequence[CalendarSpreadObservation],
    replay_config: ReplayConfig,
) -> RollExpiryEffectsResult:
    """AEGIS-081. ``observations`` must be the same
    :class:`CalendarSpreadObservation` sequence built under ``policy`` (same
    dates, same chain) -- this function does not rebuild it, only slices and
    measures it against the independently-computed roll dates.
    """
    audit = build_roll_audit(chain, policy, roll_observations, prices, dates)
    roll_dates = tuple(record.as_of for record in audit)

    slice_of: dict[date, RollExpirySlice] = {
        observation.as_of: _classify(observation.as_of, roll_dates) for observation in observations
    }

    replay = replay_strategy(observations, replay_config)
    entry_dates = {rt.entry_as_of for rt in replay.round_trips}
    exit_dates = {rt.exit_as_of for rt in replay.round_trips}
    if replay.open_position_entry_as_of is not None:
        entry_dates.add(replay.open_position_entry_as_of)

    far_expiries = {
        observation.far_contract_id: chain.lookup(observation.far_contract_id).expiry
        for observation in observations
    }
    features_by_date = {
        f.as_of: f for f in compute_term_structure_features(observations, far_expiries)
    }

    metrics: list[RollExpirySliceMetrics] = []
    for slice_kind in RollExpirySlice:
        members = [o for o in observations if slice_of[o.as_of] == slice_kind]
        if not members:
            metrics.append(
                RollExpirySliceMetrics(
                    slice=slice_kind,
                    observation_count=0,
                    mean_spread=None,
                    mean_expiry_distance_days=None,
                    entry_count=0,
                    exit_count=0,
                )
            )
            continue
        mean_spread = sum((o.spread for o in members), start=Decimal(0)) / len(members)
        mean_distance = sum(
            features_by_date[o.as_of].expiry_distance_days for o in members
        ) / len(members)
        metrics.append(
            RollExpirySliceMetrics(
                slice=slice_kind,
                observation_count=len(members),
                mean_spread=mean_spread,
                mean_expiry_distance_days=mean_distance,
                entry_count=sum(1 for o in members if o.as_of in entry_dates),
                exit_count=sum(1 for o in members if o.as_of in exit_dates),
            )
        )

    return RollExpiryEffectsResult(
        roll_policy_name=type(policy).__name__, roll_dates=roll_dates, slices=tuple(metrics)
    )
