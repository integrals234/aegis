"""M2 slice 2 — AEGIS-012: contract symbols, expiry metadata and lifecycle.

The acceptance is "contract lookup and expiry-boundary tests pass". Lookup is
tested in test_futures_chain.py; this file is the expiry-boundary half, plus
construction validation and the canonical representation.
"""

from __future__ import annotations

from datetime import date, timedelta

import pytest
from futures.contracts import (
    Contract,
    ContractLifecycle,
    InvalidContract,
    SettlementType,
    lifecycle_index,
    lifecycle_state,
)
from futures.identifiers import ContractId

pytestmark = pytest.mark.unit


def make_contract(
    first_trade_date: date = date(2025, 3, 20),
    last_trade_date: date = date(2026, 3, 20),
    expiry: date = date(2026, 3, 20),
    settlement_type: SettlementType = SettlementType.CASH,
) -> Contract:
    return Contract(
        contract_id=ContractId("SYNX", "EQX", 2026, 3),
        first_trade_date=first_trade_date,
        last_trade_date=last_trade_date,
        expiry=expiry,
        settlement_type=settlement_type,
    )


# --------------------------------------------------------------- construction


def test_canonical_representation_delegates_to_contract_id():
    contract = make_contract()
    assert contract.canonical == "SYNX:EQX:2026H"
    assert str(contract) == "SYNX:EQX:2026H"


@pytest.mark.parametrize(
    "kwargs",
    [
        # last_trade_date before first_trade_date
        {"first_trade_date": date(2026, 1, 1), "last_trade_date": date(2025, 12, 31)},
        # expiry before last_trade_date
        {"last_trade_date": date(2026, 3, 21), "expiry": date(2026, 3, 20)},
        # expiry before first_trade_date entirely
        {"first_trade_date": date(2026, 4, 1), "last_trade_date": date(2026, 4, 1),
         "expiry": date(2026, 3, 20)},
    ],
)
def test_constructor_rejects_inconsistent_dates(kwargs):
    with pytest.raises(InvalidContract):
        make_contract(**kwargs)


def test_constructor_accepts_all_dates_equal():
    """The degenerate but valid case: a contract that lists, trades and
    settles on the same day. Not forbidden by the frozen acceptance."""
    same_day = date(2026, 3, 20)
    contract = make_contract(first_trade_date=same_day, last_trade_date=same_day, expiry=same_day)
    assert contract.first_trade_date == contract.last_trade_date == contract.expiry


def test_constructor_rejects_wrong_types():
    with pytest.raises(InvalidContract):
        Contract(
            contract_id="not-a-contract-id",  # type: ignore[arg-type]
            first_trade_date=date(2025, 1, 1),
            last_trade_date=date(2026, 1, 1),
            expiry=date(2026, 1, 1),
            settlement_type=SettlementType.CASH,
        )
    with pytest.raises(InvalidContract):
        make_contract(settlement_type="cash")  # type: ignore[arg-type]  # a str, not the enum
    with pytest.raises(InvalidContract):
        make_contract(expiry="2026-03-20")  # type: ignore[arg-type]  # a str, not a date


# ---------------------------------------------------------------- lifecycle


def test_lifecycle_boundaries_cash_settled_same_day():
    """last_trade_date == expiry: the interesting boundary ADR-0015 calls out
    — LAST_TRADING_DAY must win over SETTLED on that exact day."""
    contract = make_contract(
        first_trade_date=date(2025, 3, 20), last_trade_date=date(2026, 3, 20),
        expiry=date(2026, 3, 20),
    )
    cases = {
        date(2025, 3, 19): ContractLifecycle.PRE_LISTING,
        date(2025, 3, 20): ContractLifecycle.ACTIVE,
        date(2026, 3, 19): ContractLifecycle.ACTIVE,
        date(2026, 3, 20): ContractLifecycle.LAST_TRADING_DAY,  # == expiry too
        date(2026, 3, 21): ContractLifecycle.SETTLED,
        date(2027, 1, 1): ContractLifecycle.SETTLED,
    }
    for as_of, expected in cases.items():
        assert lifecycle_state(contract, as_of) == expected, as_of


def test_lifecycle_boundaries_physical_settlement_gap():
    """last_trade_date < expiry: EXPIRED has non-zero width."""
    contract = make_contract(
        first_trade_date=date(2025, 1, 1), last_trade_date=date(2026, 1, 17),
        expiry=date(2026, 1, 20), settlement_type=SettlementType.PHYSICAL,
    )
    cases = {
        date(2024, 12, 31): ContractLifecycle.PRE_LISTING,
        date(2026, 1, 1): ContractLifecycle.ACTIVE,
        date(2026, 1, 16): ContractLifecycle.ACTIVE,
        date(2026, 1, 17): ContractLifecycle.LAST_TRADING_DAY,
        date(2026, 1, 18): ContractLifecycle.EXPIRED,
        date(2026, 1, 19): ContractLifecycle.EXPIRED,
        date(2026, 1, 20): ContractLifecycle.SETTLED,
        date(2026, 1, 21): ContractLifecycle.SETTLED,
    }
    for as_of, expected in cases.items():
        assert lifecycle_state(contract, as_of) == expected, as_of


def test_lifecycle_all_dates_equal_is_always_last_trading_day():
    same_day = date(2026, 3, 20)
    contract = make_contract(first_trade_date=same_day, last_trade_date=same_day, expiry=same_day)
    assert lifecycle_state(contract, same_day) == ContractLifecycle.LAST_TRADING_DAY
    assert lifecycle_state(contract, same_day - timedelta(days=1)) == ContractLifecycle.PRE_LISTING
    assert lifecycle_state(contract, same_day + timedelta(days=1)) == ContractLifecycle.SETTLED


def test_lifecycle_state_rejects_non_date_as_of():
    with pytest.raises(InvalidContract):
        lifecycle_state(make_contract(), "2026-03-20")  # type: ignore[arg-type]


def test_lifecycle_state_is_pure_and_repeatable():
    """No hidden state: identical inputs, identical output, every time."""
    contract = make_contract()
    as_of = date(2026, 1, 1)
    results = {lifecycle_state(contract, as_of) for _ in range(50)}
    assert results == {ContractLifecycle.ACTIVE}


def test_lifecycle_index_is_exhaustive_and_ordered():
    assert [lifecycle_index(s) for s in ContractLifecycle] == list(range(len(ContractLifecycle)))


def test_exactly_five_lifecycle_states_and_two_settlement_types():
    """ADR-0015 approved exactly these; a sixth would be scope creep."""
    assert {s.value for s in ContractLifecycle} == {
        "pre_listing", "active", "last_trading_day", "expired", "settled",
    }
    assert {s.value for s in SettlementType} == {"cash", "physical"}
