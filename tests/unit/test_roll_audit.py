"""M2 slice 8 -- AEGIS-023: roll audit report.

Golden fixture against the real committed EQX chain, reusing the exact
volume-crossover scenario `tests/unit/test_roll_volume_crossover.py`
already proves (rolls on day 6 with persistence_days=2), so the audit
record's own math can be checked by hand against a known case.
"""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.roll.policy import RollObservation
from futures.roll.volume_crossover import VolumeCrossoverPolicy
from futures.roll_audit import build_roll_audit, render_human_readable, to_machine_readable
from futures.series import InvalidAdjustment, PriceObservation

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


def _crossover_fixture() -> tuple[list[RollObservation], list[PriceObservation], list[date]]:
    dates = [BASE_DAY + timedelta(days=i) for i in range(10)]
    observations = []
    prices = []
    for i, day in enumerate(dates):
        observations.append(RollObservation(FRONT, day, 1000 - i * 80, None))
        observations.append(RollObservation(DEFERRED, day, 200 + i * 90, None))
        prices.append(PriceObservation(FRONT, day, Decimal(100 + i)))
        prices.append(PriceObservation(DEFERRED, day, Decimal(150 + i)))
    return observations, prices, dates


def test_audit_records_the_known_roll_with_correct_gap_and_ratio(chain: ContractChain) -> None:
    observations, prices, dates = _crossover_fixture()
    policy = VolumeCrossoverPolicy(persistence_days=2)
    records = build_roll_audit(chain, policy, observations, prices, dates)

    assert len(records) == 1
    record = records[0]
    assert record.as_of == BASE_DAY + timedelta(days=6)  # matches test_roll_volume_crossover.py
    assert record.old_contract == FRONT
    assert record.new_contract == DEFERRED
    assert record.old_price_at_roll == Decimal(106)
    assert record.new_price_at_roll == Decimal(156)
    assert record.raw_gap == Decimal(50)
    assert record.ratio_at_roll == Decimal(156) / Decimal(106)
    assert record.trigger == "VolumeCrossoverPolicy"


def test_no_roll_no_records(chain: ContractChain) -> None:
    dates = [BASE_DAY + timedelta(days=i) for i in range(3)]
    observations = [RollObservation(FRONT, d, 1000, None) for d in dates] + [
        RollObservation(DEFERRED, d, 200, None) for d in dates
    ]
    prices = [PriceObservation(FRONT, d, Decimal(100)) for d in dates]
    policy = VolumeCrossoverPolicy(persistence_days=2)
    assert build_roll_audit(chain, policy, observations, prices, dates) == ()


def test_missing_roll_date_dual_quote_raises(chain: ContractChain) -> None:
    observations, prices, dates = _crossover_fixture()
    # Drop the outgoing contract's price on the roll date itself.
    roll_day = BASE_DAY + timedelta(days=6)
    filtered_prices = [p for p in prices if not (p.contract_id == FRONT and p.session_date == roll_day)]
    policy = VolumeCrossoverPolicy(persistence_days=2)
    with pytest.raises(InvalidAdjustment):
        build_roll_audit(chain, policy, observations, filtered_prices, dates)


def test_machine_and_human_readable_share_one_data_source(chain: ContractChain) -> None:
    observations, prices, dates = _crossover_fixture()
    policy = VolumeCrossoverPolicy(persistence_days=2)
    records = build_roll_audit(chain, policy, observations, prices, dates)

    machine = to_machine_readable(records)
    human = render_human_readable(records)
    assert len(machine) == len(records) == 1
    assert machine[0]["old_contract"] == "SYNX:EQX:2026H"
    assert machine[0]["new_contract"] == "SYNX:EQX:2026M"
    assert "SYNX:EQX:2026H" in human
    assert "SYNX:EQX:2026M" in human
    assert "VolumeCrossoverPolicy" in human
