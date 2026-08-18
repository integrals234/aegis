"""Calendar spread construction from the M2 futures stack (AEGIS-076).

Reuses :class:`~futures.chain.ContractChain` and contract lifecycle (M2 slice
2), a :class:`~futures.roll.policy.RollPolicy` (M2 slice 6) for near-leg
selection, and :class:`~futures.series.PriceObservation` (M2 slice 7) for raw
prices. No contract, chain or roll machinery is duplicated here.

# Provenance, and the far leg's price honestly

The near leg is real in both identity and price: the roll policy's own
``front_contract()`` choice, priced from ``data_samples/futures/bars/*``, the
same normalized settlement-price bars M2 already ships.

The far leg's *identity* is also real -- the next contract listed in the same
chain after the near contract, on the same date, a fact ``ContractChain``
already knows.

The far leg's *price* is **observed when an observation for that contract on
that date exists in the supplied price series, and constructed only when one
does not**. Which of the two happened is recorded per observation, in
:attr:`CalendarSpreadObservation.far_price_provenance` and the boolean
:attr:`CalendarSpreadObservation.far_price_observed` -- a reader never has to
guess. Nothing is presented as observed market data for the far leg unless it
genuinely was (ADR-0025).

The constructed fallback exists because ``data_samples/futures/bars/*``
carries only one contract's bar history per product, so the M4 demo's own EQX
chain has no observed far-leg price available: there, the far price comes from
a fixed, documented, caller-supplied basis rule
(:class:`ConstructedBasisRule`) applied to the near leg's own observed price.

**Why preferring the observed price matters, beyond provenance.** A purely
additive constructed basis makes ``far_price - near_price`` algebraically
independent of *which* contract is currently front, so the spread -- and
therefore any strategy signal computed from it -- becomes invariant to the
roll policy. That invariance is an artifact of the construction, not a fact
about calendar spreads, and it would silently flatten AEGIS-024's
roll-method sensitivity comparison wherever real far-leg prices were in fact
available. Using them when they exist removes the artifact.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal

from futures.chain import ContractChain
from futures.identifiers import ContractId
from futures.roll.policy import RollObservation, RollPolicy, listed_contract_ids_at
from futures.series import PriceObservation

__all__ = [
    "CalendarSpreadDataError",
    "CalendarSpreadObservation",
    "ConstructedBasisRule",
    "build_calendar_spread_observations",
]


class CalendarSpreadDataError(ValueError):
    """A date could not produce a spread observation: no front contract, no
    next-listed contract to serve as the far leg, or no observed near-leg
    price. Raised rather than silently skipped -- a gap here is a fact about
    the input data, not something this module papers over."""


@dataclass(frozen=True, slots=True)
class ConstructedBasisRule:
    """A fixed, documented additive basis applied to the near leg's own
    observed price to derive the far leg's price (see module docstring).

    ``basis_units_by_index`` is indexed by the observation's position within
    the date sequence being built (0-based, wrapping via modulo) -- not by
    calendar date -- so the same rule can be reused across date ranges of any
    length. A single-element tuple is a constant basis.
    """

    basis_units_by_index: tuple[Decimal, ...]
    description: str

    def __post_init__(self) -> None:
        if not self.basis_units_by_index:
            raise ValueError("basis_units_by_index must be non-empty")

    def far_price(self, near_price: Decimal, observation_index: int) -> Decimal:
        basis = self.basis_units_by_index[observation_index % len(self.basis_units_by_index)]
        return near_price + basis


@dataclass(frozen=True, slots=True)
class CalendarSpreadObservation:
    """One date's near/far calendar-spread observation, with the provenance
    AEGIS-076's frozen acceptance requires: the research dataset can
    reproduce every spread observation from this record alone plus the
    committed inputs it names."""

    as_of: date
    near_contract_id: ContractId
    far_contract_id: ContractId
    near_price: Decimal
    far_price: Decimal
    roll_policy_name: str
    far_price_provenance: str
    contract_steps: int
    far_price_observed: bool = False

    @property
    def spread(self) -> Decimal:
        return self.far_price - self.near_price


def _next_listed_contract(
    chain: ContractChain, as_of: date, near: ContractId, steps: int
) -> ContractId:
    listed = listed_contract_ids_at(chain, as_of)
    if near not in listed:
        raise CalendarSpreadDataError(f"{near.canonical} is not listed on {as_of.isoformat()}")
    far_index = listed.index(near) + steps
    if far_index >= len(listed):
        raise CalendarSpreadDataError(
            f"chain has no contract {steps} step(s) beyond {near.canonical} on "
            f"{as_of.isoformat()} ({len(listed)} listed)"
        )
    return listed[far_index]


def build_calendar_spread_observations(
    chain: ContractChain,
    policy: RollPolicy,
    roll_observations: Sequence[RollObservation],
    near_prices: Sequence[PriceObservation],
    as_of_dates: Sequence[date],
    basis_rule: ConstructedBasisRule,
    far_contract_steps: int = 1,
) -> tuple[CalendarSpreadObservation, ...]:
    """AEGIS-076. ``far_contract_steps`` names how many chain positions past
    the near (front) contract the far leg sits; 1 -- the default -- is "the
    very next listed contract", the calendar-spread convention this module
    implements. Raises :class:`CalendarSpreadDataError` rather than silently
    skipping a date the inputs cannot honestly cover.
    """
    if far_contract_steps < 1:
        raise ValueError(f"far_contract_steps must be >= 1, got {far_contract_steps}")

    price_by_key = {(p.contract_id, p.session_date): p.price for p in near_prices}
    policy_name = type(policy).__name__

    observations: list[CalendarSpreadObservation] = []
    for index, as_of in enumerate(sorted(as_of_dates)):
        near_id = policy.front_contract(chain, roll_observations, as_of)
        if near_id is None:
            raise CalendarSpreadDataError(f"no front contract selected for {as_of.isoformat()}")
        far_id = _next_listed_contract(chain, as_of, near_id, far_contract_steps)

        key = (near_id, as_of)
        if key not in price_by_key:
            raise CalendarSpreadDataError(
                f"no observed price for {near_id.canonical} on {as_of.isoformat()}"
            )
        near_price = price_by_key[key]

        # Prefer the far contract's OWN observed price when the supplied
        # series carries one; fall back to the documented construction only
        # when it does not (see module docstring).
        far_key = (far_id, as_of)
        if far_key in price_by_key:
            far_price = price_by_key[far_key]
            far_observed = True
            far_provenance = (
                f"observed: {far_id.canonical}'s own price on {as_of.isoformat()} from the "
                "supplied price series"
            )
        else:
            far_price = basis_rule.far_price(near_price, index)
            far_observed = False
            far_provenance = basis_rule.description

        observations.append(
            CalendarSpreadObservation(
                as_of=as_of,
                near_contract_id=near_id,
                far_contract_id=far_id,
                near_price=near_price,
                far_price=far_price,
                roll_policy_name=policy_name,
                far_price_provenance=far_provenance,
                contract_steps=far_contract_steps,
                far_price_observed=far_observed,
            )
        )
    return tuple(observations)
