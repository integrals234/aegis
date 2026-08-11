"""M2 slice 6 -- AEGIS-016: volume-crossover roll policy.

Crossover, reversal, and missing-volume cases, per the frozen acceptance.
"""

from __future__ import annotations

from datetime import date, timedelta

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.roll.policy import RollObservation
from futures.roll.volume_crossover import VolumeCrossoverPolicy

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


def _obs(day_offset: int, front_volume: int | None, deferred_volume: int | None) -> list[RollObservation]:
    day = BASE_DAY + timedelta(days=day_offset)
    entries = []
    if front_volume is not None:
        entries.append(RollObservation(FRONT, day, front_volume, None))
    if deferred_volume is not None:
        entries.append(RollObservation(DEFERRED, day, deferred_volume, None))
    return entries


def test_rejects_persistence_days_below_one() -> None:
    with pytest.raises(ValueError, match="persistence_days"):
        VolumeCrossoverPolicy(persistence_days=0)


def test_stays_on_front_before_crossover(chain: ContractChain) -> None:
    observations = _obs(0, 1000, 200)
    policy = VolumeCrossoverPolicy(persistence_days=1)
    assert policy.front_contract(chain, observations, BASE_DAY) == FRONT


def test_rolls_immediately_with_persistence_one(chain: ContractChain) -> None:
    observations = _obs(0, 100, 500)  # deferred already exceeds front
    policy = VolumeCrossoverPolicy(persistence_days=1)
    assert policy.front_contract(chain, observations, BASE_DAY) == DEFERRED


def test_does_not_roll_until_persistence_threshold_reached() -> None:
    chain = ContractChain("SYNX", "EQX")
    for contract_id, expiry in ((FRONT, date(2026, 3, 20)), (DEFERRED, date(2026, 6, 20))):
        chain.add(
            Contract(
                contract_id=contract_id,
                first_trade_date=date(2025, 1, 1),
                last_trade_date=expiry,
                expiry=expiry,
                settlement_type=SettlementType.CASH,
            )
        )
    observations: list[RollObservation] = []
    # Deferred exceeds front on days 0 and 1 (only 2 consecutive so far).
    observations += _obs(0, 1000, 1200)
    observations += _obs(1, 950, 1100)
    policy = VolumeCrossoverPolicy(persistence_days=3)
    as_of = BASE_DAY + timedelta(days=1)
    assert policy.front_contract(chain, observations, as_of) == FRONT  # not persisted yet

    observations += _obs(2, 900, 1300)  # third consecutive day
    as_of = BASE_DAY + timedelta(days=2)
    assert policy.front_contract(chain, observations, as_of) == DEFERRED


def test_reversal_resets_the_persistence_count(chain: ContractChain) -> None:
    observations: list[RollObservation] = []
    observations += _obs(0, 1000, 1200)  # deferred ahead
    observations += _obs(1, 1000, 800)  # reversal: front ahead again
    observations += _obs(2, 1000, 1200)  # deferred ahead again (only 1 consecutive)
    policy = VolumeCrossoverPolicy(persistence_days=2)
    as_of = BASE_DAY + timedelta(days=2)
    # The most recent comparable day shows deferred ahead, but only for one
    # day since the day-1 reversal -- not enough to persist at threshold 2.
    assert policy.front_contract(chain, observations, as_of) == FRONT


def test_missing_volume_day_is_skipped_not_treated_as_zero(chain: ContractChain) -> None:
    observations: list[RollObservation] = []
    observations += _obs(0, 1000, 1200)  # deferred ahead, day 0
    observations += _obs(1, 900, None)  # deferred volume missing entirely on day 1
    observations += _obs(2, 800, 1300)  # deferred ahead, day 2
    policy = VolumeCrossoverPolicy(persistence_days=2)
    as_of = BASE_DAY + timedelta(days=2)
    # Day 1 is skipped (not comparable); the two comparable days (0 and 2)
    # both show deferred ahead, so persistence-2 is satisfied.
    assert policy.front_contract(chain, observations, as_of) == DEFERRED


def test_missing_volume_on_front_is_also_skipped(chain: ContractChain) -> None:
    observations: list[RollObservation] = []
    observations += _obs(0, None, 1200)  # front volume missing
    observations += _obs(1, 900, 1300)
    policy = VolumeCrossoverPolicy(persistence_days=2)
    as_of = BASE_DAY + timedelta(days=1)
    # Only one comparable day exists; persistence-2 cannot be satisfied.
    assert policy.front_contract(chain, observations, as_of) == FRONT


def test_no_listed_contract_returns_none() -> None:
    empty_chain = ContractChain("SYNX", "EQX")
    policy = VolumeCrossoverPolicy()
    assert policy.front_contract(empty_chain, [], BASE_DAY) is None


def test_single_listed_contract_is_front_with_no_deferred_to_compare() -> None:
    single = ContractChain("SYNX", "EQX")
    single.add(
        Contract(
            contract_id=FRONT,
            first_trade_date=date(2025, 1, 1),
            last_trade_date=date(2026, 3, 20),
            expiry=date(2026, 3, 20),
            settlement_type=SettlementType.CASH,
        )
    )
    policy = VolumeCrossoverPolicy()
    assert policy.front_contract(single, [], BASE_DAY) == FRONT
