"""M2 slice 6 -- AEGIS-018: composite liquidity-score roll policy.

Score components, weights and the selected contract must be reproducible
and auditable via the exposed breakdown.
"""

from __future__ import annotations

from datetime import date
from decimal import Decimal

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.roll.liquidity_score import LiquidityScorePolicy
from futures.roll.policy import RollObservation, listed_contract_ids_at

pytestmark = pytest.mark.unit

A = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
B = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
AS_OF = date(2026, 1, 10)


@pytest.fixture
def chain() -> ContractChain:
    result = ContractChain("SYNX", "EQX")
    for contract_id, expiry in ((A, date(2026, 3, 20)), (B, date(2026, 6, 20))):
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


def test_rejects_negative_weights() -> None:
    with pytest.raises(ValueError):
        LiquidityScorePolicy(volume_weight=Decimal("-0.1"), open_interest_weight=Decimal("1.1"))


def test_rejects_both_weights_zero() -> None:
    with pytest.raises(ValueError):
        LiquidityScorePolicy(volume_weight=Decimal(0), open_interest_weight=Decimal(0))


def test_higher_combined_score_wins(chain: ContractChain) -> None:
    observations = [
        RollObservation(A, AS_OF, volume=1000, open_interest=2000),
        RollObservation(B, AS_OF, volume=3000, open_interest=1000),
    ]
    policy = LiquidityScorePolicy(volume_weight=Decimal("0.7"), open_interest_weight=Decimal("0.3"))
    assert policy.front_contract(chain, observations, AS_OF) == B


def test_score_is_exactly_reproducible(chain: ContractChain) -> None:
    observations = [
        RollObservation(A, AS_OF, volume=1000, open_interest=2000),
        RollObservation(B, AS_OF, volume=3000, open_interest=1000),
    ]
    policy = LiquidityScorePolicy(volume_weight=Decimal("0.7"), open_interest_weight=Decimal("0.3"))
    listed = listed_contract_ids_at(chain, AS_OF)
    first = policy.score_breakdown(listed, observations, AS_OF)
    second = policy.score_breakdown(listed, observations, AS_OF)
    assert first == second


def test_breakdown_components_sum_to_the_score(chain: ContractChain) -> None:
    observations = [
        RollObservation(A, AS_OF, volume=1000, open_interest=2000),
        RollObservation(B, AS_OF, volume=3000, open_interest=1000),
    ]
    policy = LiquidityScorePolicy(volume_weight=Decimal("0.6"), open_interest_weight=Decimal("0.4"))
    listed = listed_contract_ids_at(chain, AS_OF)
    for entry in policy.score_breakdown(listed, observations, AS_OF):
        expected = (
            policy.volume_weight * entry.volume_component
            + policy.open_interest_weight * entry.open_interest_component
        )
        assert entry.score == expected


def test_missing_metric_contributes_zero_not_an_error(chain: ContractChain) -> None:
    observations = [
        RollObservation(A, AS_OF, volume=1000, open_interest=None),
        RollObservation(B, AS_OF, volume=None, open_interest=1000),
    ]
    policy = LiquidityScorePolicy(volume_weight=Decimal("0.5"), open_interest_weight=Decimal("0.5"))
    listed = listed_contract_ids_at(chain, AS_OF)
    breakdown = {b.contract_id: b for b in policy.score_breakdown(listed, observations, AS_OF)}
    assert breakdown[A].open_interest_component == Decimal(0)
    assert breakdown[B].volume_component == Decimal(0)


def test_no_observations_all_scores_zero_ties_broken_by_chronological_order(
    chain: ContractChain,
) -> None:
    policy = LiquidityScorePolicy()
    front = policy.front_contract(chain, [], AS_OF)
    assert front == A  # nearest-expiry contract wins the tie


def test_no_listed_contract_returns_none() -> None:
    empty_chain = ContractChain("SYNX", "EQX")
    policy = LiquidityScorePolicy()
    assert policy.front_contract(empty_chain, [], AS_OF) is None


def test_uses_the_most_recent_observation_on_or_before_as_of(chain: ContractChain) -> None:
    observations = [
        RollObservation(A, date(2026, 1, 1), volume=100, open_interest=100),
        RollObservation(A, date(2026, 1, 9), volume=5000, open_interest=5000),  # most recent <= AS_OF
        RollObservation(B, AS_OF, volume=100, open_interest=100),
    ]
    policy = LiquidityScorePolicy()
    assert policy.front_contract(chain, observations, AS_OF) == A
