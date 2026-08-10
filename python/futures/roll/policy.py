"""The roll-policy interface and shared machinery (AEGIS-015..018).

``ContractChain`` (M2 slice 2, `python/futures/chain.py`) stays policy-neutral
by design -- it orders and looks up contracts, and deliberately has no
"front contract" method, because fixed-days, volume-crossover,
open-interest-crossover and liquidity-score are four different, mutually
incompatible answers to "which contract is front on a date" (ADR-0015).
This module is where that decision finally gets made, one policy at a time.

Every policy in this package is a pure function of
``(chain, observations, as_of)``: no wall clock, no mutable state, no hidden
global. A policy is either a frozen, parameter-holding dataclass (every one
here is) or a bare function with the same signature.
"""

from __future__ import annotations

from collections.abc import Callable, Sequence
from dataclasses import dataclass
from datetime import date
from typing import Protocol

from futures.chain import ContractChain
from futures.identifiers import ContractId

__all__ = [
    "RollObservation",
    "RollPolicy",
    "crossover_confirmed",
    "latest_metric_on_or_before",
    "listed_contract_ids_at",
]


@dataclass(frozen=True, slots=True)
class RollObservation:
    """One session's volume/open-interest observation for one contract.

    Deliberately narrower than a `futures_bar.v1` record (`python/futures/
    schema.py`): a roll policy needs only these four fields, and coupling
    this layer to the wider normalized-bar shape would pull ingestion's
    concerns into roll for no reason. A caller that has `futures_bar.v1`
    records builds `RollObservation`s from them explicitly.
    """

    contract_id: ContractId
    session_date: date
    volume: int | None
    open_interest: int | None


class RollPolicy(Protocol):
    """Structural interface every roll policy satisfies. Not a base class --
    each policy is its own frozen dataclass; this is what a caller that
    accepts "any roll policy" type-checks against.
    """

    def front_contract(
        self, chain: ContractChain, observations: Sequence[RollObservation], as_of: date
    ) -> ContractId | None: ...


def listed_contract_ids_at(chain: ContractChain, as_of: date) -> list[ContractId]:
    """Contract IDs actually listed on ``as_of``, in chronological order.

    Every policy in this package chooses only from this set -- never a
    contract the chain does not consider tradeable that day, and never by
    guessing at nearest-expiry (that would silently reintroduce the "fifth
    roll policy" ADR-0015 refused to bake into `ContractChain` itself).
    """
    return [contract.contract_id for contract in chain.listed_at(as_of)]


def latest_metric_on_or_before(
    observations: Sequence[RollObservation],
    contract_id: ContractId,
    metric: Callable[[RollObservation], int | None],
    as_of: date,
) -> int | None:
    """The most recent non-null ``metric`` value for ``contract_id`` on or
    before ``as_of``, or ``None`` if there is no such observation. Never
    substitutes a different contract's or a different metric's value --
    missing stays missing."""
    candidates = [
        (observation.session_date, value)
        for observation in observations
        if observation.contract_id == contract_id
        and observation.session_date <= as_of
        and (value := metric(observation)) is not None
    ]
    if not candidates:
        return None
    return max(candidates)[1]


def crossover_confirmed(
    observations: Sequence[RollObservation],
    metric: Callable[[RollObservation], int | None],
    current: ContractId,
    deferred: ContractId,
    as_of: date,
    persistence_days: int,
) -> bool:
    """Has ``deferred`` exceeded ``current`` on ``metric`` for
    ``persistence_days`` consecutive *observed* sessions, ending at the most
    recent session on or before ``as_of`` where both contracts have a
    non-null value for ``metric``?

    Walking backward from ``as_of``: a session where either side is missing
    the metric is skipped entirely -- it neither extends nor breaks the
    streak (missing volume/OI is never treated as zero). A session where
    ``deferred`` does *not* exceed ``current`` breaks the streak immediately
    -- a reversal at or before the persistence threshold resets the count,
    it does not merely pause it.
    """
    if persistence_days < 1:
        raise ValueError(f"persistence_days must be >= 1, got {persistence_days}")

    current_by_date = {
        observation.session_date: value
        for observation in observations
        if observation.contract_id == current
        and observation.session_date <= as_of
        and (value := metric(observation)) is not None
    }
    deferred_by_date = {
        observation.session_date: value
        for observation in observations
        if observation.contract_id == deferred
        and observation.session_date <= as_of
        and (value := metric(observation)) is not None
    }
    comparable_dates = sorted(
        (d for d in current_by_date if d in deferred_by_date), reverse=True
    )

    streak = 0
    for session_date in comparable_dates:
        if deferred_by_date[session_date] > current_by_date[session_date]:
            streak += 1
            if streak >= persistence_days:
                return True
        else:
            break
    return False
