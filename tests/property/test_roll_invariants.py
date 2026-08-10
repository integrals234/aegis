"""M2 slice 6 -- invariants every roll policy must hold, not just the golden
fixtures in tests/unit/test_roll_*.py.

* **Reproducibility**: identical inputs produce an identical decision.
* **No roll before chain validity**: a policy never selects a contract
  that is not actually listed (`ContractChain.listed_at`) on the date in
  question.
* **Stable result independent of insertion/observation order**: shuffling
  the observation list must not change the outcome.

Deliberately does *not* assert "front contract's volume/OI is always the
highest" or any other economically-loaded monotonic property -- the M2 plan
of record warns against asserting economically false invariants merely
because they are easy to write; a policy choosing a less-liquid contract is
not a bug, since fixed-days is explicitly not liquidity-based at all.
"""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType, lifecycle_state
from futures.identifiers import ContractId
from futures.roll.fixed_days import FixedDaysPolicy
from futures.roll.liquidity_score import LiquidityScorePolicy
from futures.roll.oi_crossover import OpenInterestCrossoverPolicy
from futures.roll.policy import RollObservation
from futures.roll.volume_crossover import VolumeCrossoverPolicy
from hypothesis import given, settings
from hypothesis import strategies as st

pytestmark = pytest.mark.property

CONTRACT_A = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
CONTRACT_B = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
BASE_DAY = date(2026, 1, 1)


def _chain() -> ContractChain:
    result = ContractChain("SYNX", "EQX")
    for contract_id, expiry in ((CONTRACT_A, date(2026, 3, 20)), (CONTRACT_B, date(2026, 6, 20))):
        result.add(
            Contract(
                contract_id=contract_id,
                first_trade_date=date(2025, 1, 1),
                last_trade_date=expiry,
                expiry=expiry,
                settlement_type=SettlementType.CASH,
            )
        )
    return result


metrics = st.one_of(st.none(), st.integers(min_value=0, max_value=1_000_000))
day_offsets = st.integers(min_value=0, max_value=60)

observation_lists = st.lists(
    st.tuples(st.sampled_from([CONTRACT_A, CONTRACT_B]), day_offsets, metrics, metrics),
    max_size=20,
)


def _build_observations(entries: list[tuple[ContractId, int, int | None, int | None]]) -> list[RollObservation]:
    return [
        RollObservation(contract_id, BASE_DAY + timedelta(days=offset), volume, oi)
        for contract_id, offset, volume, oi in entries
    ]


policies = (
    FixedDaysPolicy(days_before_expiry=10),
    VolumeCrossoverPolicy(persistence_days=2),
    OpenInterestCrossoverPolicy(persistence_days=2),
    LiquidityScorePolicy(volume_weight=Decimal("0.5"), open_interest_weight=Decimal("0.5")),
)


@given(entries=observation_lists, as_of_offset=day_offsets, policy=st.sampled_from(policies))
@settings(max_examples=100)
def test_result_is_reproducible(
    entries: list[tuple[ContractId, int, int | None, int | None]], as_of_offset: int, policy: object
) -> None:
    chain = _chain()
    observations = _build_observations(entries)
    as_of = BASE_DAY + timedelta(days=as_of_offset)
    first = policy.front_contract(chain, observations, as_of)  # type: ignore[attr-defined]
    second = policy.front_contract(chain, observations, as_of)  # type: ignore[attr-defined]
    assert first == second


@given(entries=observation_lists, as_of_offset=day_offsets, policy=st.sampled_from(policies))
@settings(max_examples=100)
def test_selected_contract_is_actually_listed(
    entries: list[tuple[ContractId, int, int | None, int | None]], as_of_offset: int, policy: object
) -> None:
    chain = _chain()
    observations = _build_observations(entries)
    as_of = BASE_DAY + timedelta(days=as_of_offset)
    front = policy.front_contract(chain, observations, as_of)  # type: ignore[attr-defined]
    if front is None:
        assert not chain.listed_at(as_of)
        return
    contract = chain.lookup(front)
    state = lifecycle_state(contract, as_of)
    assert state not in (state.PRE_LISTING, state.SETTLED)


@given(entries=observation_lists, as_of_offset=day_offsets, policy=st.sampled_from(policies))
@settings(max_examples=100)
def test_result_independent_of_observation_order(
    entries: list[tuple[ContractId, int, int | None, int | None]], as_of_offset: int, policy: object
) -> None:
    chain = _chain()
    observations = _build_observations(entries)
    as_of = BASE_DAY + timedelta(days=as_of_offset)
    forward = policy.front_contract(chain, observations, as_of)  # type: ignore[attr-defined]
    backward = policy.front_contract(chain, list(reversed(observations)), as_of)  # type: ignore[attr-defined]
    assert forward == backward
