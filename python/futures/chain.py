"""Ordered contract chains and lookup (AEGIS-012).

A :class:`ContractChain` is every known :class:`~futures.contracts.Contract`
for one product, in chronological order. It answers "what contracts exist for
this product, and in what order" — nothing about which one is currently
tradeable as "the front month".

That omission is deliberate, not an oversight to fix later. The M2 plan of
record is explicit that *the roll policy decides which contract is front on a
date, not the chain* — fixed-days, volume-crossover, open-interest-crossover
and liquidity-score are four different, mutually incompatible answers to that
question, and they are M2 slice 6. A chain method that picked one by nearest
expiry would silently be a fifth, undocumented roll policy.
"""

from __future__ import annotations

import bisect
from collections.abc import Iterator
from dataclasses import dataclass
from datetime import date
from typing import Final

from futures.contracts import Contract, ContractLifecycle, lifecycle_state
from futures.identifiers import ContractId

__all__ = ["ContractChain", "DuplicateContract", "UnknownContract"]

# Not "listed" (AEGIS-012's contract-lookup surface, no roll opinion): a
# contract before its first trade date, or on/after settlement, is not a
# tradeable instance of anything.
_NOT_LISTED: Final[frozenset[ContractLifecycle]] = frozenset(
    {ContractLifecycle.PRE_LISTING, ContractLifecycle.SETTLED}
)


class DuplicateContract(ValueError):
    """A contract with an identity already in the chain was added again."""


class UnknownContract(KeyError):
    """A lookup named a contract identity the chain does not hold."""


@dataclass(frozen=True, slots=True)
class _Sortable:
    """A contract paired with its chronological sort key.

    ``(year, month)`` rather than ``ContractId``'s field order: ``ContractId``
    orders lexicographically by dataclass field, so it sorts venue and
    product_root before year, which is wrong for a single-product chain.
    """

    order: tuple[int, int]
    contract: Contract


class ContractChain:
    """All known contracts for one ``(venue, product_root)``, chronologically
    ordered by expiry year and month.

    Sorted on construction and after every insertion, never by insertion order
    or dict iteration — the ordering is a property of the contracts' own
    identities, so a chain built from the same contracts in any order compares
    equal in iteration.
    """

    def __init__(self, venue: str, product_root: str) -> None:
        self._venue = venue
        self._product_root = product_root
        self._entries: list[_Sortable] = []
        self._by_id: dict[ContractId, Contract] = {}

    @property
    def venue(self) -> str:
        return self._venue

    @property
    def product_root(self) -> str:
        return self._product_root

    def __len__(self) -> int:
        return len(self._entries)

    def __iter__(self) -> Iterator[Contract]:
        return (entry.contract for entry in self._entries)

    def __contains__(self, contract_id: ContractId) -> bool:
        return contract_id in self._by_id

    def add(self, contract: Contract) -> None:
        """Insert ``contract``, keeping chronological order.

        Rejects a contract from a different product outright: a chain mixing
        two products would make "chronological order" ambiguous the moment two
        products' expiries interleave.
        """
        cid = contract.contract_id
        if (cid.venue, cid.product_root) != (self._venue, self._product_root):
            raise ValueError(
                f"contract {cid.canonical} belongs to {cid.venue}:{cid.product_root}, "
                f"not this chain's {self._venue}:{self._product_root}"
            )
        if cid in self._by_id:
            raise DuplicateContract(f"{cid.canonical} is already in this chain")

        key = (cid.year, cid.month)
        entry = _Sortable(order=key, contract=contract)
        index = bisect.bisect_left([e.order for e in self._entries], key)
        self._entries.insert(index, entry)
        self._by_id[cid] = contract

    def lookup(self, contract_id: ContractId) -> Contract:
        """Look up a contract by exact identity. Raises, never returns ``None``
        — a caller that meant to check presence should use ``in`` instead, so a
        typo'd identity fails loudly rather than as a silently absent
        observation two steps downstream."""
        try:
            return self._by_id[contract_id]
        except KeyError:
            raise UnknownContract(
                f"no contract {contract_id.canonical} in {self._venue}:{self._product_root}"
            ) from None

    def listed_at(self, as_of: date) -> list[Contract]:
        """Contracts whose lifecycle is neither ``PRE_LISTING`` nor ``SETTLED``
        on ``as_of`` — a lifecycle fact, in chronological order. Not a "front
        contract" opinion: it may return zero, one or several contracts."""
        return [
            entry.contract
            for entry in self._entries
            if lifecycle_state(entry.contract, as_of) not in _NOT_LISTED
        ]
