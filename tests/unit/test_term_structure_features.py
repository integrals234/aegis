"""AEGIS-077 -- term-structure features over known curve shapes."""

from __future__ import annotations

from datetime import date
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from research.calendar_spread import CalendarSpreadObservation
from research.term_structure import CurveState, classify_curve_state, compute_term_structure_features

pytestmark = pytest.mark.unit

NEAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
FAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
FAR_EXPIRY = date(2026, 6, 20)


def _observation(as_of: date, near_price: Decimal, far_price: Decimal) -> CalendarSpreadObservation:
    return CalendarSpreadObservation(
        as_of=as_of,
        near_contract_id=NEAR,
        far_contract_id=FAR,
        near_price=near_price,
        far_price=far_price,
        roll_policy_name="FixedDaysPolicy",
        far_price_provenance="test fixture",
        contract_steps=1,
    )


@pytest.mark.parametrize(
    ("near_price", "far_price", "expected_state"),
    [
        (Decimal("100.00"), Decimal("105.00"), CurveState.CONTANGO),
        (Decimal("100.00"), Decimal("95.00"), CurveState.BACKWARDATION),
        (Decimal("100.00"), Decimal("100.00"), CurveState.FLAT),
    ],
)
def test_classifies_known_curve_shapes(
    near_price: Decimal, far_price: Decimal, expected_state: CurveState
) -> None:
    assert classify_curve_state(far_price - near_price) == expected_state


def test_computes_carry_expiry_distance_and_roll_context() -> None:
    as_of = date(2026, 3, 1)
    observation = _observation(as_of, Decimal("100.00"), Decimal("110.00"))

    (features,) = compute_term_structure_features([observation], {FAR: FAR_EXPIRY})

    assert features.as_of == as_of
    assert features.near_contract_id == NEAR
    assert features.far_contract_id == FAR
    assert features.spread == Decimal("10.00")
    assert features.carry == Decimal("10.00") / Decimal("100.00")
    assert features.curve_state == CurveState.CONTANGO
    assert features.expiry_distance_days == (FAR_EXPIRY - as_of).days
    assert features.roll_policy_name == "FixedDaysPolicy"


def test_zero_near_price_raises_rather_than_dividing_by_zero() -> None:
    observation = _observation(date(2026, 3, 1), Decimal(0), Decimal("10.00"))
    with pytest.raises(ValueError, match="zero near price"):
        compute_term_structure_features([observation], {FAR: FAR_EXPIRY})


def test_sequence_of_observations_produces_one_feature_set_each() -> None:
    observations = [
        _observation(date(2026, 3, 1), Decimal("100.00"), Decimal("105.00")),
        _observation(date(2026, 3, 2), Decimal("100.00"), Decimal("95.00")),
    ]
    features = compute_term_structure_features(observations, {FAR: FAR_EXPIRY})
    assert [f.curve_state for f in features] == [CurveState.CONTANGO, CurveState.BACKWARDATION]
