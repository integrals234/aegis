"""Continuous futures series and adjustment methods (AEGIS-019..022).

Built on top of *explicit roll selections* -- a caller-supplied
``Mapping[date, ContractId]`` naming the front contract on every date the
series should cover -- rather than this module calling a
`futures.roll.RollPolicy` itself. That keeps roll *selection* (M2 slice 6)
and roll *consumption* (this slice) as separate, independently testable
concerns: series construction cannot depend on which policy produced the
selections, and a roll audit (a later slice) can replay a fixed selection
sequence without re-running policy logic.

`Decimal` throughout, matching the M2 slice 2 precedent for `tick_size`/
`multiplier`: an adjustment factor or offset is not merely reported, it
changes every downstream price, and a `float` computation would make that
result platform-dependent.

# The back-adjustment convention, precisely (ADR-0017's slice 7 addendum)

Both additive and ratio adjustment use the **same-day dual quote** at each
roll: the outgoing (old front) contract's own price *on the roll date
itself*, not a proxy from the day before. This is what makes the
reconciliation property in AEGIS-022 hold exactly: the adjusted return
across a roll boundary equals the *old* contract's own realized return for
that specific day -- not zero, and not the new contract's return.

This deliberately does **not** make the adjusted price level perfectly
continuous at the splice (adjusted[roll day] generally differs from
adjusted[day before] by exactly that realized return). A convention that
forced continuity there would need a *different* gap formula -- the new
contract's roll-day price against the old contract's previous-day price --
which would silently discard the old contract's real price movement on the
roll day itself and replace it with an invented zero. That invented zero is
the "artificial roll jump" AEGIS-022 asks to avoid; preserving the real,
realized return is what "avoiding" it means here. The most recent segment
is always the unadjusted reference (offset 0 / factor 1); every earlier
segment is shifted by the cumulative sum/product of every roll's own
same-day gap or ratio.
"""

from __future__ import annotations

import itertools
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal

from futures.identifiers import ContractId

__all__ = [
    "AdditiveAdjustedObservation",
    "ContinuousObservation",
    "InvalidAdjustment",
    "MissingPrice",
    "PriceObservation",
    "RatioAdjustedObservation",
    "ReturnObservation",
    "build_additive_adjusted_series",
    "build_ratio_adjusted_series",
    "build_return_stream",
    "build_unadjusted_series",
    "roll_boundary_prices",
]


class MissingPrice(ValueError):
    """A selected front contract has no price observation on a date the
    unadjusted series must cover. Raised rather than skipped -- a gap in
    the continuous series is a fact, not something to silently paper over."""


class InvalidAdjustment(ValueError):
    """An additive/ratio adjustment or return could not be computed: a
    missing roll-date price, a non-positive price where a ratio needs one,
    or a zero adjusted price a return would divide by. Never silently
    substituted or skipped."""


@dataclass(frozen=True, slots=True)
class PriceObservation:
    """One contract's price on one session date -- the raw input series
    adjustment reads from, independent of which contract was ever "front"."""

    contract_id: ContractId
    session_date: date
    price: Decimal


@dataclass(frozen=True, slots=True)
class ContinuousObservation:
    """AEGIS-019: one point of the unadjusted continuous series.

    ``contract_id`` is always present -- the provenance AEGIS-019 requires:
    it must always be possible to answer which underlying contract generated
    this observation. ``is_roll_point`` is true exactly when the front
    contract differs from the previous observation's.
    """

    as_of: date
    contract_id: ContractId
    raw_price: Decimal
    is_roll_point: bool


@dataclass(frozen=True, slots=True)
class AdditiveAdjustedObservation:
    """AEGIS-020: one point of the difference (additive) back-adjusted series."""

    as_of: date
    contract_id: ContractId
    raw_price: Decimal
    adjustment_offset: Decimal
    adjusted_price: Decimal
    is_roll_point: bool


@dataclass(frozen=True, slots=True)
class RatioAdjustedObservation:
    """AEGIS-021: one point of the ratio (multiplicative) back-adjusted series."""

    as_of: date
    contract_id: ContractId
    raw_price: Decimal
    adjustment_factor: Decimal
    adjusted_price: Decimal
    is_roll_point: bool


@dataclass(frozen=True, slots=True)
class ReturnObservation:
    """AEGIS-022: one simple (proportional) return, ``adjusted[t] /
    adjusted[t-1] - 1``, computed from an adjusted series. Intended for the
    ratio (multiplicative) convention, whose adjusted prices are directly
    proportional-return-comparable; the additive convention's reconciliation
    is naturally expressed as a price *difference* instead (see
    `tests/unit/test_series_reconciliation.py`), not through this function.
    """

    as_of: date
    contract_id: ContractId
    is_roll_point: bool
    simple_return: Decimal


def build_unadjusted_series(
    front_by_date: Mapping[date, ContractId],
    prices: Sequence[PriceObservation],
) -> tuple[ContinuousObservation, ...]:
    """AEGIS-019: the unadjusted continuous series over explicit roll
    selections. Every date in ``front_by_date`` must have a matching price
    observation for the contract selected that date -- a missing one raises
    :class:`MissingPrice` rather than silently skipping the date, which
    would erase the fact that a gap exists.
    """
    lookup = {(p.contract_id, p.session_date): p.price for p in prices}
    observations: list[ContinuousObservation] = []
    previous_contract: ContractId | None = None
    for as_of in sorted(front_by_date):
        contract_id = front_by_date[as_of]
        key = (contract_id, as_of)
        if key not in lookup:
            raise MissingPrice(
                f"no price observation for {contract_id.canonical} on {as_of.isoformat()}, "
                "the selected front contract that date"
            )
        observations.append(
            ContinuousObservation(
                as_of=as_of,
                contract_id=contract_id,
                raw_price=lookup[key],
                is_roll_point=previous_contract is not None and previous_contract != contract_id,
            )
        )
        previous_contract = contract_id
    return tuple(observations)


def roll_boundary_prices(
    unadjusted: Sequence[ContinuousObservation],
    index: int,
    prices: Sequence[PriceObservation],
) -> tuple[ContractId, date, Decimal, Decimal]:
    """Shared roll-boundary lookup for additive/ratio adjustment: the
    outgoing contract, the roll date, and (old, new) prices on that date --
    the new price from the already-built series, the old price from the raw
    observations (the same-day dual quote adjustment needs). Public because
    `python/futures/roll_audit.py` (M2 slice 8) reuses the identical
    same-day-dual-quote lookup rather than re-deriving it."""
    lookup = {(p.contract_id, p.session_date): p.price for p in prices}
    old_contract = unadjusted[index].contract_id
    roll_day = unadjusted[index + 1].as_of
    key = (old_contract, roll_day)
    if key not in lookup:
        raise InvalidAdjustment(
            f"adjustment needs {old_contract.canonical}'s own price on {roll_day.isoformat()} "
            "(the roll date itself, a same-day dual quote) to compute the roll gap/ratio; "
            "none was supplied"
        )
    return old_contract, roll_day, lookup[key], unadjusted[index + 1].raw_price


def build_additive_adjusted_series(
    unadjusted: Sequence[ContinuousObservation],
    prices: Sequence[PriceObservation],
) -> tuple[AdditiveAdjustedObservation, ...]:
    """AEGIS-020: difference (additive) back-adjustment.

    At each roll, ``gap = new_contract_price_at_roll - old_contract_price_at_roll``
    (both on the roll date itself). Every earlier observation's offset is the
    cumulative sum of every gap at or after it, walking backward; the most
    recent segment carries offset zero. This is what makes the roll-day
    reconciliation property in AEGIS-022 hold exactly (see module docstring):
    the adjusted return across the roll equals the outgoing contract's own
    realized return that day.
    """
    if not unadjusted:
        return ()
    count = len(unadjusted)
    offsets: list[Decimal] = [Decimal(0)] * count
    cumulative = Decimal(0)
    for index in range(count - 2, -1, -1):
        if unadjusted[index + 1].contract_id != unadjusted[index].contract_id:
            _, _, old_price, new_price = roll_boundary_prices(unadjusted, index, prices)
            cumulative += new_price - old_price
        offsets[index] = cumulative
    return tuple(
        AdditiveAdjustedObservation(
            as_of=observation.as_of,
            contract_id=observation.contract_id,
            raw_price=observation.raw_price,
            adjustment_offset=offsets[i],
            adjusted_price=observation.raw_price + offsets[i],
            is_roll_point=observation.is_roll_point,
        )
        for i, observation in enumerate(unadjusted)
    )


def build_ratio_adjusted_series(
    unadjusted: Sequence[ContinuousObservation],
    prices: Sequence[PriceObservation],
) -> tuple[RatioAdjustedObservation, ...]:
    """AEGIS-021: ratio (multiplicative) back-adjustment.

    At each roll, ``ratio = new_contract_price_at_roll / old_contract_price_at_roll``
    (both on the roll date itself, the same same-day dual quote AEGIS-020
    uses). Every earlier observation's factor is the cumulative product of
    every ratio at or after it; the most recent segment carries factor one.

    Raises :class:`InvalidAdjustment` -- never silently divides by zero or
    invents a replacement -- when the roll-date price for the outgoing
    contract is missing, or is not strictly positive (a ratio against a
    zero or negative price is not a meaningful proportional adjustment).
    """
    if not unadjusted:
        return ()
    count = len(unadjusted)
    factors: list[Decimal] = [Decimal(1)] * count
    cumulative = Decimal(1)
    for index in range(count - 2, -1, -1):
        if unadjusted[index + 1].contract_id != unadjusted[index].contract_id:
            old_contract, roll_day, old_price, new_price = roll_boundary_prices(unadjusted, index, prices)
            if old_price <= 0:
                raise InvalidAdjustment(
                    f"cannot compute a ratio against a non-positive price ({old_price}) for "
                    f"{old_contract.canonical} on {roll_day.isoformat()}"
                )
            cumulative *= new_price / old_price
        factors[index] = cumulative
    return tuple(
        RatioAdjustedObservation(
            as_of=observation.as_of,
            contract_id=observation.contract_id,
            raw_price=observation.raw_price,
            adjustment_factor=factors[i],
            adjusted_price=observation.raw_price * factors[i],
            is_roll_point=observation.is_roll_point,
        )
        for i, observation in enumerate(unadjusted)
    )


def build_return_stream(
    adjusted: Sequence[AdditiveAdjustedObservation | RatioAdjustedObservation],
) -> tuple[ReturnObservation, ...]:
    """AEGIS-022: simple (proportional) returns from an adjusted series.

    Raises :class:`InvalidAdjustment` on a zero adjusted price rather than
    dividing by it. No synthetic index is built -- the M2 plan of record
    marks one optional and out of scope unless separately required, and
    none of AEGIS-019..022's frozen acceptance text names one.
    """
    returns: list[ReturnObservation] = []
    for previous, current in itertools.pairwise(adjusted):
        if previous.adjusted_price == 0:
            raise InvalidAdjustment(
                f"cannot compute a return: adjusted price is zero on {previous.as_of.isoformat()}"
            )
        returns.append(
            ReturnObservation(
                as_of=current.as_of,
                contract_id=current.contract_id,
                is_roll_point=current.is_roll_point,
                simple_return=(current.adjusted_price / previous.adjusted_price) - 1,
            )
        )
    return tuple(returns)
