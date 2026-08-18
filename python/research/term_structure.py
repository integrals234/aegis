"""Term-structure features over calendar-spread observations (AEGIS-077).

Built directly on :class:`~research.calendar_spread.CalendarSpreadObservation`
-- no new price or contract machinery, only features computed from what that
module already returns.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal
from enum import StrEnum

from futures.identifiers import ContractId

from research.calendar_spread import CalendarSpreadObservation

__all__ = [
    "CurveState",
    "TermStructureFeatures",
    "classify_curve_state",
    "compute_term_structure_features",
]


class CurveState(StrEnum):
    """AEGIS-077: contango/backwardation/flat, from the near/far spread's
    own sign -- ``far > near`` is the textbook contango (upward-sloping)
    definition, ``far < near`` backwardation."""

    CONTANGO = "contango"
    BACKWARDATION = "backwardation"
    FLAT = "flat"


def classify_curve_state(spread: Decimal) -> CurveState:
    if spread > 0:
        return CurveState.CONTANGO
    if spread < 0:
        return CurveState.BACKWARDATION
    return CurveState.FLAT


@dataclass(frozen=True, slots=True)
class TermStructureFeatures:
    """One date's term-structure feature set (AEGIS-077): the near/far
    spread, its proportional carry, the curve's contango/backwardation/flat
    state, the far contract's distance to its own expiry, and the roll
    context (which policy chose the near leg) that produced it."""

    as_of: date
    near_contract_id: ContractId
    far_contract_id: ContractId
    spread: Decimal
    carry: Decimal
    curve_state: CurveState
    expiry_distance_days: int
    roll_policy_name: str


def compute_term_structure_features(
    observations: Sequence[CalendarSpreadObservation],
    far_expiries: Mapping[ContractId, date],
) -> tuple[TermStructureFeatures, ...]:
    """AEGIS-077. ``carry`` is the spread expressed as a fraction of the near
    leg's own price (``spread / near_price``) -- the proportional cost (or
    benefit) of carrying the position to the far contract, a documented
    simplification that ignores financing/storage costs no frozen
    requirement asks this module to model. ``far_expiries`` supplies each far
    contract's own expiry date; a contract absent from it is a caller error
    (:class:`KeyError`), not silently treated as having no expiry distance.
    """
    features: list[TermStructureFeatures] = []
    for observation in observations:
        if observation.near_price == 0:
            raise ValueError(
                f"cannot compute carry against a zero near price on {observation.as_of.isoformat()}"
            )
        far_expiry = far_expiries[observation.far_contract_id]
        features.append(
            TermStructureFeatures(
                as_of=observation.as_of,
                near_contract_id=observation.near_contract_id,
                far_contract_id=observation.far_contract_id,
                spread=observation.spread,
                carry=observation.spread / observation.near_price,
                curve_state=classify_curve_state(observation.spread),
                expiry_distance_days=(far_expiry - observation.as_of).days,
                roll_policy_name=observation.roll_policy_name,
            )
        )
    return tuple(features)
