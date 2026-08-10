"""M2 slice 2 — invariants that must hold for every contract, not a handful.

ADR-0015 makes three claims strong enough to need this layer:

* contract identity has exactly one spelling — parse(canonical) round-trips;
* lifecycle is monotonic non-decreasing as as_of increases, for ANY valid
  contract, not just the two boundary cases test_futures_contracts.py checks
  by hand;
* chain iteration order is a property of the contracts' identities, never of
  insertion order.

If any of these failed for some input nobody thought to write down, a roll
audit or a continuous series built on top of this module would silently
misattribute an observation to the wrong contract.
"""

from __future__ import annotations

from datetime import date, timedelta

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType, lifecycle_index, lifecycle_state
from futures.identifiers import ContractId
from hypothesis import given, settings
from hypothesis import strategies as st

pytestmark = pytest.mark.property

tokens = st.text(alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", min_size=1, max_size=12)
years = st.integers(min_value=1970, max_value=2999)
months = st.integers(min_value=1, max_value=12)

contract_ids = st.builds(ContractId, venue=tokens, product_root=tokens, year=years, month=months)


# ------------------------------------------------------------------- identity


@given(contract_id=contract_ids)
def test_contract_id_round_trips_through_canonical(contract_id: ContractId) -> None:
    assert ContractId.parse(contract_id.canonical) == contract_id
    assert ContractId.parse(contract_id.canonical).canonical == contract_id.canonical


@given(contract_id=contract_ids)
def test_canonical_string_always_matches_the_fixed_shape(contract_id: ContractId) -> None:
    venue, root, rest = contract_id.canonical.split(":")
    assert venue == contract_id.venue
    assert root == contract_id.product_root
    assert rest == f"{contract_id.year:04d}{contract_id.month_code}"
    assert len(rest) == 5  # 4-digit year, 1-character month code — fixed width


@given(a=contract_ids, b=contract_ids)
def test_equal_canonical_strings_imply_equal_identity(a: ContractId, b: ContractId) -> None:
    """The property AEGIS-012's provenance depends on: no two spellings for
    one contract, and no one spelling for two different contracts."""
    if a.canonical == b.canonical:
        assert a == b
    if a != b:
        assert a.canonical != b.canonical


# ------------------------------------------------------------------ lifecycle


@st.composite
def ordered_contracts(draw: st.DrawFn) -> Contract:
    """A contract whose three dates satisfy first <= last <= expiry by
    construction, so generation never wastes examples on rejects."""
    base = draw(st.dates(min_value=date(2000, 1, 1), max_value=date(2100, 1, 1)))
    gap_a = draw(st.integers(min_value=0, max_value=400))
    gap_b = draw(st.integers(min_value=0, max_value=30))
    first = base
    last = base + timedelta(days=gap_a)
    expiry = last + timedelta(days=gap_b)
    contract_id = draw(contract_ids)
    settlement = draw(st.sampled_from(list(SettlementType)))
    return Contract(
        contract_id=contract_id,
        first_trade_date=first,
        last_trade_date=last,
        expiry=expiry,
        settlement_type=settlement,
    )


@given(contract=ordered_contracts(), offsets=st.lists(st.integers(min_value=-5, max_value=5),
                                                       min_size=2, max_size=8))
@settings(max_examples=200)
def test_lifecycle_is_monotonic_non_decreasing(contract: Contract, offsets: list[int]) -> None:
    """As as_of increases, the lifecycle index never goes backwards — for any
    valid contract, any date range, not just the hand-picked boundary cases."""
    base = contract.first_trade_date
    dates = sorted({base + timedelta(days=n) for n in offsets})
    indices = [lifecycle_index(lifecycle_state(contract, d)) for d in dates]
    assert indices == sorted(indices)


@given(contract=ordered_contracts())
def test_lifecycle_state_is_always_one_of_the_five(contract: Contract) -> None:
    for offset in (-1000, -1, 0, 1, 1000):
        state = lifecycle_state(contract, contract.expiry + timedelta(days=offset))
        assert lifecycle_index(state) in range(5)


@given(contract=ordered_contracts())
def test_last_trading_day_state_occurs_on_exactly_the_last_trade_date(contract: Contract) -> None:
    """LAST_TRADING_DAY is a single day, by definition — never zero days
    (it is defined as an equality, not a range) and never more than one."""
    from futures.contracts import ContractLifecycle

    matching = [
        d
        for d in (contract.last_trade_date + timedelta(days=n) for n in range(-2, 3))
        if lifecycle_state(contract, d) == ContractLifecycle.LAST_TRADING_DAY
    ]
    assert matching == [contract.last_trade_date]


# ----------------------------------------------------------------- chain


@given(pairs=st.lists(st.tuples(years, months), min_size=1, max_size=12, unique=True))
@settings(max_examples=100)
def test_chain_iteration_order_is_independent_of_insertion_order(
    pairs: list[tuple[int, int]],
) -> None:
    def build(order: list[tuple[int, int]]) -> list[str]:
        chain = ContractChain("SYNX", "EQX")
        for year, month in order:
            expiry = date(year, month, 1)
            chain.add(
                Contract(
                    contract_id=ContractId("SYNX", "EQX", year, month),
                    first_trade_date=expiry - timedelta(days=100),
                    last_trade_date=expiry,
                    expiry=expiry,
                    settlement_type=SettlementType.CASH,
                )
            )
        return [c.contract_id.canonical for c in chain]

    forward = build(pairs)
    reversed_order = build(list(reversed(pairs)))
    assert forward == reversed_order
    # And it must actually be chronological, not merely insertion-independent.
    # Parsed back through ContractId rather than string-sorted, so this checks
    # real (year, month) order instead of re-asserting the lexicographic
    # property test_canonical_string_always_matches_the_fixed_shape already
    # covers.
    parsed = [ContractId.parse(s) for s in forward]
    assert parsed == sorted(parsed, key=lambda c: (c.year, c.month))
