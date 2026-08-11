"""M2 slice 6 -- AEGIS-017: open-interest-crossover roll policy.

Mirrors test_roll_volume_crossover.py's coverage (crossover, missing-OI)
over open interest instead of volume -- the two metrics share
`futures.roll.policy.crossover_confirmed` but are exercised as separate
policies, per the M2 plan of record.
"""

from __future__ import annotations

from datetime import date, timedelta

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.roll.oi_crossover import OpenInterestCrossoverPolicy
from futures.roll.policy import RollObservation

pytestmark = pytest.mark.unit

FRONT = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
DEFERRED = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
BASE_DAY = date(2026, 1, 1)


@pytest.fixture
def chain() -> ContractChain:
    result = ContractChain("SYNX", "EQX")
    for contract_id, expiry in ((FRONT, date(2026, 3, 20)), (DEFERRED, date(2026, 6, 20))):
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


def _obs(day_offset: int, front_oi: int | None, deferred_oi: int | None) -> list[RollObservation]:
    day = BASE_DAY + timedelta(days=day_offset)
    entries = []
    if front_oi is not None:
        entries.append(RollObservation(FRONT, day, None, front_oi))
    if deferred_oi is not None:
        entries.append(RollObservation(DEFERRED, day, None, deferred_oi))
    return entries


def test_rejects_persistence_days_below_one() -> None:
    with pytest.raises(ValueError, match="persistence_days"):
        OpenInterestCrossoverPolicy(persistence_days=0)


def test_stays_on_front_before_crossover(chain: ContractChain) -> None:
    observations = _obs(0, 5000, 1000)
    policy = OpenInterestCrossoverPolicy(persistence_days=1)
    assert policy.front_contract(chain, observations, BASE_DAY) == FRONT


def test_rolls_immediately_with_persistence_one(chain: ContractChain) -> None:
    observations = _obs(0, 500, 6000)
    policy = OpenInterestCrossoverPolicy(persistence_days=1)
    assert policy.front_contract(chain, observations, BASE_DAY) == DEFERRED


def test_missing_open_interest_day_is_skipped_not_treated_as_zero(chain: ContractChain) -> None:
    observations: list[RollObservation] = []
    observations += _obs(0, 5000, 6000)  # deferred ahead
    observations += _obs(1, 4800, None)  # deferred OI missing
    observations += _obs(2, 4600, 6500)  # deferred ahead again
    policy = OpenInterestCrossoverPolicy(persistence_days=2)
    as_of = BASE_DAY + timedelta(days=2)
    assert policy.front_contract(chain, observations, as_of) == DEFERRED


def test_volume_does_not_substitute_for_missing_open_interest(chain: ContractChain) -> None:
    """Even if volume data exists for the deferred contract, it must never
    be used in place of a missing open-interest value."""
    day = BASE_DAY
    observations = [
        RollObservation(FRONT, day, volume=100, open_interest=5000),
        RollObservation(DEFERRED, day, volume=999999, open_interest=None),  # huge volume, no OI
    ]
    policy = OpenInterestCrossoverPolicy(persistence_days=1)
    assert policy.front_contract(chain, observations, day) == FRONT


def test_reversal_resets_the_persistence_count(chain: ContractChain) -> None:
    observations: list[RollObservation] = []
    observations += _obs(0, 5000, 6000)
    observations += _obs(1, 5000, 4000)  # reversal
    observations += _obs(2, 5000, 6000)
    policy = OpenInterestCrossoverPolicy(persistence_days=2)
    as_of = BASE_DAY + timedelta(days=2)
    assert policy.front_contract(chain, observations, as_of) == FRONT


def test_no_listed_contract_returns_none() -> None:
    empty_chain = ContractChain("SYNX", "EQX")
    policy = OpenInterestCrossoverPolicy()
    assert policy.front_contract(empty_chain, [], BASE_DAY) is None
