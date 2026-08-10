"""M2 slice 8 -- AEGIS-024 (M2-owned portion): roll-method sensitivity.

Compares two roll policies directly (roll dates, adjusted-price-path
deviation) -- not a strategy comparison, which is the registered M4
residual (see `python/futures/roll_sensitivity.py`'s module docstring).
"""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.roll.fixed_days import FixedDaysPolicy
from futures.roll.policy import RollObservation
from futures.roll.volume_crossover import VolumeCrossoverPolicy
from futures.roll_sensitivity import compare_roll_methods
from futures.series import PriceObservation

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


def _fixture() -> tuple[list[RollObservation], list[PriceObservation], list[date]]:
    # DEFERRED's price gap over FRONT is deliberately non-constant (quadratic,
    # not linear) so two policies rolling on different dates produce adjusted
    # price paths that genuinely diverge -- a constant gap would make every
    # additive back-adjustment numerically identical regardless of roll date,
    # which would defeat the point of this fixture.
    dates = [BASE_DAY + timedelta(days=i) for i in range(10)]
    observations = []
    prices = []
    for i, day in enumerate(dates):
        observations.append(RollObservation(FRONT, day, 1000 - i * 80, None))
        observations.append(RollObservation(DEFERRED, day, 200 + i * 90, None))
        prices.append(PriceObservation(FRONT, day, Decimal(100 + i)))
        prices.append(PriceObservation(DEFERRED, day, Decimal(150 + i * i)))
    return observations, prices, dates


def test_two_policies_with_different_roll_dates_are_flagged(chain: ContractChain) -> None:
    observations, prices, dates = _fixture()
    policies = {
        "volume": VolumeCrossoverPolicy(persistence_days=2),  # rolls day 6
        "fixed_far": FixedDaysPolicy(days_before_expiry=200),  # rolls immediately (day 0)
    }
    comparisons = compare_roll_methods(chain, policies, observations, prices, dates)

    assert len(comparisons) == 1
    comparison = comparisons[0]
    assert comparison.policy_a == "fixed_far"
    assert comparison.policy_b == "volume"
    assert comparison.roll_dates_differ is True
    assert comparison.max_abs_price_deviation > 0


def test_identical_policy_instances_show_no_divergence(chain: ContractChain) -> None:
    observations, prices, dates = _fixture()
    policies = {
        "a": VolumeCrossoverPolicy(persistence_days=2),
        "b": VolumeCrossoverPolicy(persistence_days=2),
    }
    comparisons = compare_roll_methods(chain, policies, observations, prices, dates)

    assert len(comparisons) == 1
    comparison = comparisons[0]
    assert comparison.roll_dates_differ is False
    assert comparison.max_abs_price_deviation == 0
    assert comparison.mean_abs_price_deviation == 0


def test_three_policies_produce_three_pairwise_comparisons(chain: ContractChain) -> None:
    observations, prices, dates = _fixture()
    policies = {
        "a": VolumeCrossoverPolicy(persistence_days=1),
        "b": VolumeCrossoverPolicy(persistence_days=2),
        "c": FixedDaysPolicy(days_before_expiry=200),
    }
    comparisons = compare_roll_methods(chain, policies, observations, prices, dates)
    assert len(comparisons) == 3
    pairs = {(c.policy_a, c.policy_b) for c in comparisons}
    assert pairs == {("a", "b"), ("a", "c"), ("b", "c")}
