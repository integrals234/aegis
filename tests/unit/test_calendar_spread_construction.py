"""AEGIS-076 -- calendar spread construction (M4 Batch 1)."""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.roll.fixed_days import FixedDaysPolicy
from futures.series import PriceObservation
from research.calendar_spread import (
    CalendarSpreadDataError,
    ConstructedBasisRule,
    build_calendar_spread_observations,
)

pytestmark = pytest.mark.unit

NEAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
FAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
BASE_DAY = date(2026, 1, 1)


@pytest.fixture
def chain() -> ContractChain:
    result = ContractChain("SYNX", "EQX")
    for contract_id, expiry in ((NEAR, date(2026, 3, 20)), (FAR, date(2026, 6, 20))):
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


def _dates(count: int) -> list[date]:
    return [BASE_DAY + timedelta(days=i) for i in range(count)]


def test_reproduces_every_spread_observation_from_committed_inputs(chain: ContractChain) -> None:
    dates = _dates(3)
    near_prices = [PriceObservation(NEAR, day, Decimal("100.00") + i) for i, day in enumerate(dates)]
    basis = ConstructedBasisRule(basis_units_by_index=(Decimal("5.00"),), description="flat basis")

    observations = build_calendar_spread_observations(
        chain=chain,
        policy=FixedDaysPolicy(days_before_expiry=0),
        roll_observations=(),
        near_prices=near_prices,
        as_of_dates=dates,
        basis_rule=basis,
    )

    assert len(observations) == 3
    for i, observation in enumerate(observations):
        assert observation.as_of == dates[i]
        assert observation.near_contract_id == NEAR
        assert observation.far_contract_id == FAR
        assert observation.near_price == Decimal("100.00") + i
        assert observation.far_price == observation.near_price + Decimal("5.00")
        assert observation.spread == Decimal("5.00")
        assert observation.roll_policy_name == "FixedDaysPolicy"
        assert observation.far_price_provenance == "flat basis"


def test_far_price_is_never_presented_as_observed(chain: ContractChain) -> None:
    """AEGIS-076's provenance requirement, the negative case: every
    observation must name the basis rule that constructed its far price --
    never silently omit that it was constructed rather than observed."""
    dates = _dates(1)
    near_prices = [PriceObservation(NEAR, dates[0], Decimal("100.00"))]
    basis = ConstructedBasisRule(
        basis_units_by_index=(Decimal("1.00"),), description="NOT observed: constructed for testing"
    )
    (observation,) = build_calendar_spread_observations(
        chain=chain,
        policy=FixedDaysPolicy(days_before_expiry=0),
        roll_observations=(),
        near_prices=near_prices,
        as_of_dates=dates,
        basis_rule=basis,
    )
    assert "NOT observed" in observation.far_price_provenance


def test_missing_near_price_raises_rather_than_skipping(chain: ContractChain) -> None:
    dates = _dates(1)
    basis = ConstructedBasisRule(basis_units_by_index=(Decimal("1.00"),), description="basis")
    with pytest.raises(CalendarSpreadDataError, match="no observed price"):
        build_calendar_spread_observations(
            chain=chain,
            policy=FixedDaysPolicy(days_before_expiry=0),
            roll_observations=(),
            near_prices=(),
            as_of_dates=dates,
            basis_rule=basis,
        )


def test_no_far_contract_available_raises(chain: ContractChain) -> None:
    """The near contract is the LAST listed one on this date (FixedDaysPolicy
    rolled the chain to FAR), so there is no next-listed contract for the far
    leg -- a fact the chain genuinely does not have, not something to guess
    at."""
    as_of = date(2026, 3, 15)  # Within 30 days of NEAR's expiry: rolls to FAR.
    near_prices = [PriceObservation(FAR, as_of, Decimal("100.00"))]
    basis = ConstructedBasisRule(basis_units_by_index=(Decimal("1.00"),), description="basis")
    with pytest.raises(CalendarSpreadDataError, match="no contract"):
        build_calendar_spread_observations(
            chain=chain,
            policy=FixedDaysPolicy(days_before_expiry=30),
            roll_observations=(),
            near_prices=near_prices,
            as_of_dates=[as_of],
            basis_rule=basis,
        )


def test_basis_rule_wraps_by_observation_index(chain: ContractChain) -> None:
    dates = _dates(4)
    near_prices = [PriceObservation(NEAR, day, Decimal(100)) for day in dates]
    basis = ConstructedBasisRule(
        basis_units_by_index=(Decimal("1.00"), Decimal("2.00")), description="alternating"
    )
    observations = build_calendar_spread_observations(
        chain=chain,
        policy=FixedDaysPolicy(days_before_expiry=0),
        roll_observations=(),
        near_prices=near_prices,
        as_of_dates=dates,
        basis_rule=basis,
    )
    assert [o.spread for o in observations] == [
        Decimal("1.00"),
        Decimal("2.00"),
        Decimal("1.00"),
        Decimal("2.00"),
    ]
