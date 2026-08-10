"""M2 slice 6 -- AEGIS-015: fixed-days-to-expiry roll policy.

Golden fixtures against the real, committed EQX contract chain (M2 slice 2)
-- exact roll dates, not just "a roll happens somewhere".
"""

from __future__ import annotations

from datetime import date
from pathlib import Path

import pytest
from futures.chain import ContractChain
from futures.roll.fixed_days import FixedDaysPolicy

pytestmark = pytest.mark.unit

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def eqx_chain() -> ContractChain:
    import sys

    sys.path.insert(0, str(ROOT / "tools"))
    from make_futures_fixtures import load_family

    venue, product_root, contracts = load_family(ROOT / "data_samples/futures/eqx.json")
    chain = ContractChain(venue, product_root)
    for contract in contracts:
        chain.add(contract)
    return chain


def test_rejects_negative_days_before_expiry() -> None:
    with pytest.raises(ValueError, match="days_before_expiry"):
        FixedDaysPolicy(days_before_expiry=-1)


def test_zero_days_before_expiry_is_valid() -> None:
    FixedDaysPolicy(days_before_expiry=0)


def test_no_listed_contract_returns_none(eqx_chain: ContractChain) -> None:
    policy = FixedDaysPolicy(days_before_expiry=5)
    # Well before the first contract's first_trade_date (2025-03-20).
    assert policy.front_contract(eqx_chain, [], date(2020, 1, 1)) is None


def test_stays_on_front_contract_while_outside_the_roll_window(eqx_chain: ContractChain) -> None:
    """2026-03-14 is 6 calendar days before the front contract's 2026-03-20
    expiry -- outside a 5-day window, so no roll yet."""
    policy = FixedDaysPolicy(days_before_expiry=5)
    front = policy.front_contract(eqx_chain, [], date(2026, 3, 14))
    assert front is not None
    assert front.canonical == "SYNX:EQX:2026H"


def test_rolls_exactly_at_the_inclusive_boundary(eqx_chain: ContractChain) -> None:
    """2026-03-15 is exactly 5 calendar days before expiry -- the inclusive
    boundary triggers the roll to the next listed contract."""
    policy = FixedDaysPolicy(days_before_expiry=5)
    front = policy.front_contract(eqx_chain, [], date(2026, 3, 15))
    assert front is not None
    assert front.canonical == "SYNX:EQX:2026M"


def test_stays_rolled_after_the_boundary(eqx_chain: ContractChain) -> None:
    policy = FixedDaysPolicy(days_before_expiry=5)
    front = policy.front_contract(eqx_chain, [], date(2026, 3, 16))
    assert front is not None
    assert front.canonical == "SYNX:EQX:2026M"


def test_zero_day_policy_stays_front_the_day_before_expiry(eqx_chain: ContractChain) -> None:
    policy = FixedDaysPolicy(days_before_expiry=0)
    front = policy.front_contract(eqx_chain, [], date(2026, 3, 19))
    assert front is not None
    assert front.canonical == "SYNX:EQX:2026H"


def test_zero_day_policy_rolls_exactly_on_expiry_day(eqx_chain: ContractChain) -> None:
    policy = FixedDaysPolicy(days_before_expiry=0)
    # 2026-03-20 is the expiry itself -- LastTradingDay for the old contract,
    # but the inclusive days_to_expiry <= 0 rule already rolls that day.
    front = policy.front_contract(eqx_chain, [], date(2026, 3, 20))
    assert front is not None
    assert front.canonical == "SYNX:EQX:2026M"


def test_ignores_observations_argument(eqx_chain: ContractChain) -> None:
    """AEGIS-015 needs only expiry dates -- passing observations must not
    change the outcome."""
    policy = FixedDaysPolicy(days_before_expiry=5)
    as_of = date(2026, 3, 10)
    assert policy.front_contract(eqx_chain, [], as_of) == policy.front_contract(
        eqx_chain, [object()], as_of  # type: ignore[list-item]
    )
