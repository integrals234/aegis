"""Individual futures contracts, expiry metadata and lifecycle (AEGIS-012).

A :class:`Contract` couples a :class:`~futures.identifiers.ContractId` (venue,
product root, expiry year/month — built in M2 slice 1) to the dates and
metadata that make it a specific, tradeable instance rather than a name:
first/last trade dates, expiry, and settlement type.

# Lifecycle is a pure function, not stored state

``PreListing -> Active -> LastTradingDay -> Expired -> Settled`` is computed
from ``(contract, as_of)`` by :func:`lifecycle_state`. There is no mutable
"current state" field on :class:`Contract` to fall out of sync with the clock —
CLAUDE.md's "use stable IDs and explicit clocks; avoid hidden global state"
applies exactly as much to a Python dataclass as to the C++ core. Two calls
with the same ``as_of`` always agree, in any process, forever.

The five states are checked as an ordered chain over a *monotonically
increasing* ``as_of``, so some can have zero width without creating a gap:
when ``last_trade_date == expiry`` (the common case), the day that would be
``Expired`` never has any candidate day for it, and ``as_of == expiry``
resolves to ``LastTradingDay`` rather than skipping straight to ``Settled``.
``tests/property/test_contract_identity.py`` asserts this monotonicity
directly rather than trusting a handful of example dates.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass
from datetime import date
from typing import Final

from futures.identifiers import ContractId

__all__ = [
    "Contract",
    "ContractLifecycle",
    "InvalidContract",
    "SettlementType",
    "lifecycle_index",
    "lifecycle_state",
]


class InvalidContract(ValueError):
    """A contract's dates or metadata are inconsistent."""


class SettlementType(enum.StrEnum):
    """How the contract is settled at expiry.

    Delivery metadata beyond this is out of scope for M2 slice 2: no frozen
    AEGIS-012 acceptance criterion names a delivery-location or grade schema,
    and inventing one here would be exactly the kind of unrequested feature
    CLAUDE.md's quality order puts after correctness.
    """

    CASH = "cash"
    PHYSICAL = "physical"


class ContractLifecycle(enum.StrEnum):
    """The five lifecycle states, in their fixed chronological order.

    Exactly the sequence the M2 plan of record approved:
    PreListing -> Active -> LastTradingDay -> Expired -> Settled. No additional
    state is introduced here.
    """

    PRE_LISTING = "pre_listing"
    ACTIVE = "active"
    LAST_TRADING_DAY = "last_trading_day"
    EXPIRED = "expired"
    SETTLED = "settled"


# The fixed order lifecycle_state's chain checks in, and the order
# test_lifecycle_is_monotonic asserts as_of never revisits.
_LIFECYCLE_ORDER: Final[tuple[ContractLifecycle, ...]] = (
    ContractLifecycle.PRE_LISTING,
    ContractLifecycle.ACTIVE,
    ContractLifecycle.LAST_TRADING_DAY,
    ContractLifecycle.EXPIRED,
    ContractLifecycle.SETTLED,
)


@dataclass(frozen=True, slots=True)
class Contract:
    """One specific futures contract: identity plus the dates that govern it.

    ``expiry`` is the date the contract stops existing and final settlement
    occurs. ``last_trade_date`` may equal ``expiry`` (common) or precede it
    (a contract that stops trading before it expires); it may never follow it.
    """

    contract_id: ContractId
    first_trade_date: date
    last_trade_date: date
    expiry: date
    settlement_type: SettlementType

    def __post_init__(self) -> None:
        if not isinstance(self.contract_id, ContractId):
            raise InvalidContract(
                f"contract_id must be a ContractId, got {type(self.contract_id).__name__}"
            )
        for name, value in (
            ("first_trade_date", self.first_trade_date),
            ("last_trade_date", self.last_trade_date),
            ("expiry", self.expiry),
        ):
            if not isinstance(value, date):
                raise InvalidContract(f"{name} must be a date, got {type(value).__name__}")
        if not isinstance(self.settlement_type, SettlementType):
            raise InvalidContract(
                f"settlement_type must be a SettlementType, got "
                f"{type(self.settlement_type).__name__}"
            )
        if not self.first_trade_date <= self.last_trade_date <= self.expiry:
            raise InvalidContract(
                "dates must satisfy first_trade_date <= last_trade_date <= expiry, got "
                f"{self.first_trade_date} <= {self.last_trade_date} <= {self.expiry}"
            )

    @property
    def canonical(self) -> str:
        """Delegates to :attr:`ContractId.canonical` — identity has exactly
        one spelling, and this module does not define a second one."""
        return self.contract_id.canonical

    def __str__(self) -> str:
        return self.canonical


def lifecycle_state(contract: Contract, as_of: date) -> ContractLifecycle:
    """The contract's lifecycle state on ``as_of``. Pure; no wall clock.

    Boundary convention, evaluated as an ordered chain:

    * ``as_of < first_trade_date``                        -> ``PRE_LISTING``
    * ``first_trade_date <= as_of < last_trade_date``      -> ``ACTIVE``
    * ``as_of == last_trade_date``                         -> ``LAST_TRADING_DAY``
    * ``last_trade_date < as_of < expiry``                 -> ``EXPIRED``
    * ``as_of >= expiry``                                  -> ``SETTLED``
    """
    if not isinstance(as_of, date):
        raise InvalidContract(f"as_of must be a date, got {type(as_of).__name__}")
    if as_of < contract.first_trade_date:
        return ContractLifecycle.PRE_LISTING
    if as_of < contract.last_trade_date:
        return ContractLifecycle.ACTIVE
    if as_of == contract.last_trade_date:
        return ContractLifecycle.LAST_TRADING_DAY
    if as_of < contract.expiry:
        return ContractLifecycle.EXPIRED
    return ContractLifecycle.SETTLED


def lifecycle_index(state: ContractLifecycle) -> int:
    """Position of ``state`` in the fixed chronological order.

    Exported so property tests can assert monotonicity — "as ``as_of``
    increases, the index never decreases" — against this module's own order
    rather than a second copy of it.
    """
    try:
        return _LIFECYCLE_ORDER.index(state)
    except ValueError:  # pragma: no cover - exhaustive over the enum by construction
        raise InvalidContract(f"unknown lifecycle state: {state!r}") from None
