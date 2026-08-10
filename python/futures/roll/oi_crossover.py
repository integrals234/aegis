"""AEGIS-017 -- open-interest-crossover roll policy.

Identical persistence mechanism to `futures.roll.volume_crossover`
(AEGIS-016), over open interest instead of volume
(`futures.roll.policy.crossover_confirmed`). Missing open interest is
skipped, never substituted with volume -- the two metrics are never mixed
within one policy's decision.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date

from futures.chain import ContractChain
from futures.identifiers import ContractId
from futures.roll.policy import RollObservation, crossover_confirmed, listed_contract_ids_at

__all__ = ["OpenInterestCrossoverPolicy"]


def _open_interest(observation: RollObservation) -> int | None:
    return observation.open_interest


@dataclass(frozen=True, slots=True)
class OpenInterestCrossoverPolicy:
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
            if crossover_confirmed(
                observations, _open_interest, current, deferred, as_of, self.persistence_days
            ):
                current = deferred
            else:
                break
        return current
