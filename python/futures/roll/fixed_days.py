"""AEGIS-015 -- fixed-days-to-expiry roll policy.

Design decisions, recorded in full in ADR-0017 (do not guess silently):

* **Calendar days, not trading/session days.** AEGIS-013's calendars
  (M2 slice 3) could in principle answer "how many sessions until expiry",
  but coupling the roll layer to a specific product's session template would
  make every roll decision depend on which calendar that product happens to
  use. Calendar-day counting needs only the contract's own ``expiry`` date,
  keeping this policy's only dependency `ContractChain`/`Contract`.
* **Inclusive boundary.** A contract rolls once it is at or within
  ``days_before_expiry`` calendar days of its own expiry: ``days_to_expiry
  <= days_before_expiry`` triggers the roll, not ``<``. One rule for every
  ``N``, including the edge case: ``days_before_expiry = 0`` rolls exactly
  on expiry day itself (``days_to_expiry == 0``), not the day after --
  the contract is still nominally listed (``LastTradingDay``) that day, but
  the *next* contract is already front from that day onward.
* **Missing-session behaviour is not applicable**: this policy has no
  session concept to be missing one of.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date

from futures.chain import ContractChain
from futures.identifiers import ContractId
from futures.roll.policy import RollObservation, listed_contract_ids_at

__all__ = ["FixedDaysPolicy"]


@dataclass(frozen=True, slots=True)
class FixedDaysPolicy:
    """Roll to the next listed contract once the current front is within
    ``days_before_expiry`` calendar days (inclusive) of its own expiry."""

    days_before_expiry: int

    def __post_init__(self) -> None:
        if self.days_before_expiry < 0:
            raise ValueError(f"days_before_expiry must be >= 0, got {self.days_before_expiry}")

    def front_contract(
        self, chain: ContractChain, observations: Sequence[RollObservation], as_of: date
    ) -> ContractId | None:
        del observations  # this policy needs only expiry dates, not volume/OI
        listed = listed_contract_ids_at(chain, as_of)
        if not listed:
            return None
        for contract_id in listed:  # nearest expiry first
            contract = chain.lookup(contract_id)
            days_to_expiry = (contract.expiry - as_of).days
            if days_to_expiry > self.days_before_expiry:
                return contract_id
        # Every listed contract is within its own roll window -- the
        # furthest-out one is the least-imminent choice actually available.
        return listed[-1]
