"""AEGIS-016 -- volume-crossover roll policy.

Rolls from the current front to the next listed (deferred) contract once the
deferred contract's volume has exceeded the front's for
``persistence_days`` consecutive *observed* sessions
(`futures.roll.policy.crossover_confirmed`). A day where either side is
missing volume is skipped, never treated as zero; a reversal before the
persistence threshold is reached resets the count rather than merely
pausing it.

A chain listing more than two contracts is walked pairwise, front vs. next
deferred, same as a real continuous-roll walk: the policy never jumps two
contracts ahead in one decision.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date

from futures.chain import ContractChain
from futures.identifiers import ContractId
from futures.roll.policy import RollObservation, crossover_confirmed, listed_contract_ids_at

__all__ = ["VolumeCrossoverPolicy"]


def _volume(observation: RollObservation) -> int | None:
    return observation.volume


@dataclass(frozen=True, slots=True)
class VolumeCrossoverPolicy:
    persistence_days: int = 1

    def __post_init__(self) -> None:
        if self.persistence_days < 1:
            raise ValueError(f"persistence_days must be >= 1, got {self.persistence_days}")

    def front_contract(
        self, chain: ContractChain, observations: Sequence[RollObservation], as_of: date
    ) -> ContractId | None:
        listed = listed_contract_ids_at(chain, as_of)
        if not listed:
            return None
        current = listed[0]
        for deferred in listed[1:]:
            if crossover_confirmed(observations, _volume, current, deferred, as_of, self.persistence_days):
                current = deferred
            else:
                break
        return current
