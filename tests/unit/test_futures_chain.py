"""M2 slice 2 — AEGIS-012: contract lookup, ordered chains.

Deliberately does not test any "front contract" behavior: ADR-0015 records
that the chain orders and looks up, and the roll policy (M2 slice 6) decides
what is front. A test asserting a "front" result here would be testing a
feature the module does not have.
"""

from __future__ import annotations

from datetime import date

import pytest
from futures.chain import ContractChain, DuplicateContract, UnknownContract
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId

pytestmark = pytest.mark.unit


def contract(year: int, month: int, venue: str = "SYNX", root: str = "EQX") -> Contract:
    expiry = date(year, month, 20)
    return Contract(
        contract_id=ContractId(venue, root, year, month),
        first_trade_date=date(year - 1, month, 20),
        last_trade_date=expiry,
        expiry=expiry,
        settlement_type=SettlementType.CASH,
    )


def test_lookup_returns_the_exact_contract():
    chain = ContractChain("SYNX", "EQX")
    c = contract(2026, 3)
    chain.add(c)
    assert chain.lookup(c.contract_id) is c


def test_lookup_of_absent_contract_raises_not_none():
    chain = ContractChain("SYNX", "EQX")
    with pytest.raises(UnknownContract):
        chain.lookup(ContractId("SYNX", "EQX", 2026, 3))


def test_membership_operator():
    chain = ContractChain("SYNX", "EQX")
    c = contract(2026, 3)
    assert c.contract_id not in chain
    chain.add(c)
    assert c.contract_id in chain


def test_duplicate_registration_is_rejected():
    chain = ContractChain("SYNX", "EQX")
    chain.add(contract(2026, 3))
    with pytest.raises(DuplicateContract):
        chain.add(contract(2026, 3))
    assert len(chain) == 1  # the rejected add must not have partially mutated the chain


def test_cross_product_contract_is_rejected():
    chain = ContractChain("SYNX", "EQX")
    with pytest.raises(ValueError, match="not this chain"):
        chain.add(contract(2026, 3, root="CLX"))
    with pytest.raises(ValueError, match="not this chain"):
        chain.add(contract(2026, 3, venue="OTHER"))
    assert len(chain) == 0


def test_iteration_is_chronological_regardless_of_add_order():
    chain = ContractChain("SYNX", "EQX")
    for year, month in [(2027, 3), (2026, 6), (2026, 3)]:  # deliberately out of order
        chain.add(contract(year, month))
    assert [c.contract_id.canonical for c in chain] == [
        "SYNX:EQX:2026H", "SYNX:EQX:2026M", "SYNX:EQX:2027H",
    ]


def test_len_and_empty_chain():
    chain = ContractChain("SYNX", "EQX")
    assert len(chain) == 0
    assert list(chain) == []


def test_listed_at_excludes_pre_listing_and_settled():
    chain = ContractChain("SYNX", "EQX")
    early = contract(2026, 3)   # first_trade 2025-03-20, expiry 2026-03-20
    late = contract(2027, 3)    # first_trade 2026-03-20, expiry 2027-03-20
    chain.add(early)
    chain.add(late)

    # Before `early` lists: nothing listed.
    assert chain.listed_at(date(2025, 3, 19)) == []
    # Only `early` has listed.
    assert [c.contract_id for c in chain.listed_at(date(2025, 6, 1))] == [early.contract_id]
    # Both listed (late lists exactly when early settles, at 2026-03-20 + 1 day).
    assert [c.contract_id for c in chain.listed_at(date(2026, 3, 21))] == [late.contract_id]
    # After both settle: nothing listed.
    assert chain.listed_at(date(2028, 1, 1)) == []


def test_chain_identity_properties():
    chain = ContractChain("SYNX", "EQX")
    assert chain.venue == "SYNX"
    assert chain.product_root == "EQX"
