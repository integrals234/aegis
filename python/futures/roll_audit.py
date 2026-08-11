"""Roll audit report (AEGIS-023).

Reuses production roll-selection (`python/futures/roll/*`, M2 slice 6) and
the same same-day-dual-quote roll-boundary lookup continuous-series
adjustment already uses (`futures.series.roll_boundary_prices`, M2 slice 7)
-- no roll or adjustment logic is re-derived here.

`python/futures/roll_sensitivity.py` (AEGIS-024, M2-owned portion) builds
on :func:`build_roll_audit` to compare policies against each other.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal
from typing import Any

from futures.chain import ContractChain
from futures.identifiers import ContractId
from futures.roll.policy import RollObservation, RollPolicy
from futures.series import PriceObservation, build_unadjusted_series, roll_boundary_prices

__all__ = [
    "RollAuditRecord",
    "build_roll_audit",
    "render_human_readable",
    "to_machine_readable",
]


@dataclass(frozen=True, slots=True)
class RollAuditRecord:
    """One roll: old/new contracts, prices, raw gap, trigger and
    adjustment. ``raw_gap``/``ratio_at_roll`` *are* the per-roll additive/
    multiplicative adjustment this roll contributes -- the same quantities
    `futures.series.build_additive_adjusted_series`/
    `build_ratio_adjusted_series` accumulate across every roll; this record
    does not duplicate that cumulative state, only this roll's own
    contribution to it.
    """

    as_of: date
    old_contract: ContractId
    new_contract: ContractId
    old_price_at_roll: Decimal
    new_price_at_roll: Decimal
    raw_gap: Decimal
    ratio_at_roll: Decimal | None  # None when old_price_at_roll <= 0
    trigger: str


def build_roll_audit(
    chain: ContractChain,
    policy: RollPolicy,
    observations: Sequence[RollObservation],
    prices: Sequence[PriceObservation],
    dates: Sequence[date],
) -> tuple[RollAuditRecord, ...]:
    """Walk ``dates`` (any order) calling ``policy.front_contract`` each
    day; one :class:`RollAuditRecord` per detected contract transition.

    Dates the policy has no listed contract for are skipped (consistent
    with `RollPolicy.front_contract` returning ``None`` for them, e.g.
    before any contract's `first_trade_date`); every other date is fed
    through the real `build_unadjusted_series`, and each transition's
    prices come from the real `roll_boundary_prices` -- the identical
    same-day-dual-quote lookup `futures.series`'s own adjustment builders
    use, not a re-derived copy of it.

    ``trigger`` is the policy's own class name -- the audit records *which*
    policy produced this roll, not a re-derived guess at why.
    """
    front_by_date: dict[date, ContractId] = {}
    for as_of in dates:
        front = policy.front_contract(chain, observations, as_of)
        if front is not None:
            front_by_date[as_of] = front

    unadjusted = build_unadjusted_series(front_by_date, prices)

    records: list[RollAuditRecord] = []
    for index in range(1, len(unadjusted)):
        if not unadjusted[index].is_roll_point:
            continue
        old_contract, roll_day, old_price, new_price = roll_boundary_prices(unadjusted, index - 1, prices)
        records.append(
            RollAuditRecord(
                as_of=roll_day,
                old_contract=old_contract,
                new_contract=unadjusted[index].contract_id,
                old_price_at_roll=old_price,
                new_price_at_roll=new_price,
                raw_gap=new_price - old_price,
                ratio_at_roll=(new_price / old_price) if old_price > 0 else None,
                trigger=type(policy).__name__,
            )
        )
    return tuple(records)


def to_machine_readable(records: Sequence[RollAuditRecord]) -> list[dict[str, Any]]:
    """The machine-readable half of AEGIS-023: one dict per record, every
    field JSON-serializable directly."""
    return [
        {
            "as_of": record.as_of.isoformat(),
            "old_contract": record.old_contract.canonical,
            "new_contract": record.new_contract.canonical,
            "old_price_at_roll": str(record.old_price_at_roll),
            "new_price_at_roll": str(record.new_price_at_roll),
            "raw_gap": str(record.raw_gap),
            "ratio_at_roll": str(record.ratio_at_roll) if record.ratio_at_roll is not None else None,
            "trigger": record.trigger,
        }
        for record in records
    ]


def render_human_readable(records: Sequence[RollAuditRecord]) -> str:
    """The human-readable half of AEGIS-023, over the *same* records
    :func:`to_machine_readable` serializes -- one data source, two views."""
    header = f"{'as_of':<12}{'old_contract':<20}{'new_contract':<20}{'raw_gap':>12}  {'ratio':>10}  trigger"
    lines = [header]
    for record in records:
        ratio_text = str(record.ratio_at_roll) if record.ratio_at_roll is not None else "n/a"
        lines.append(
            f"{record.as_of.isoformat():<12}{record.old_contract.canonical:<20}"
            f"{record.new_contract.canonical:<20}{record.raw_gap!s:>12}  {ratio_text:>10}  "
            f"{record.trigger}"
        )
    return "\n".join(lines) + "\n"
