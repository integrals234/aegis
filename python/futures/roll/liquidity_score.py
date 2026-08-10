"""AEGIS-018 -- composite liquidity-score roll policy.

Score, per listed contract, is a weighted sum of two normalized shares:

    score = volume_weight * (contract's volume / total volume across
                              currently-listed contracts)
          + open_interest_weight * (contract's open interest / total open
                                     interest across currently-listed
                                     contracts)

using each contract's most recent non-null observation on or before
``as_of`` (`futures.roll.policy.latest_metric_on_or_before`). Weights and
every intermediate component are `Decimal`, never `float` -- consistent
with M2 slice 2's tick-size/multiplier discipline -- so the score, and the
contract it selects, are exactly reproducible from the same inputs.

The front contract is the currently-listed contract with the highest score.
A tie is broken by chronological order (nearest expiry first): `max()`
over an already-chronologically-sorted sequence returns the first maximal
element, which is `listed_contract_ids_at`'s own order -- no separate
tie-break rule is needed or introduced.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal

from futures.chain import ContractChain
from futures.identifiers import ContractId
from futures.roll.policy import RollObservation, latest_metric_on_or_before, listed_contract_ids_at

__all__ = ["LiquidityScorePolicy", "ScoreBreakdown"]


@dataclass(frozen=True, slots=True)
class ScoreBreakdown:
    """Every component behind one contract's score -- exposed in full so a
    roll audit (a later slice) or evidence generator can show its work,
    not just the final selection."""

    contract_id: ContractId
    volume: int | None
    open_interest: int | None
    volume_component: Decimal
    open_interest_component: Decimal
    score: Decimal


@dataclass(frozen=True, slots=True)
class LiquidityScorePolicy:
    volume_weight: Decimal = Decimal("0.5")
    open_interest_weight: Decimal = Decimal("0.5")

    def __post_init__(self) -> None:
        if self.volume_weight < 0 or self.open_interest_weight < 0:
            raise ValueError("weights must be >= 0")
        if self.volume_weight + self.open_interest_weight == 0:
            raise ValueError("volume_weight and open_interest_weight must not both be zero")

    def score_breakdown(
        self,
        listed: Sequence[ContractId],
        observations: Sequence[RollObservation],
        as_of: date,
    ) -> tuple[ScoreBreakdown, ...]:
        volumes = {
            contract_id: latest_metric_on_or_before(observations, contract_id, lambda o: o.volume, as_of)
            for contract_id in listed
        }
        open_interests = {
            contract_id: latest_metric_on_or_before(
                observations, contract_id, lambda o: o.open_interest, as_of
            )
            for contract_id in listed
        }
        total_volume = sum(v for v in volumes.values() if v is not None)
        total_open_interest = sum(v for v in open_interests.values() if v is not None)

        breakdown = []
        for contract_id in listed:
            volume = volumes[contract_id]
            open_interest = open_interests[contract_id]
            volume_component = (
                Decimal(volume) / Decimal(total_volume)
                if volume is not None and total_volume > 0
                else Decimal(0)
            )
            open_interest_component = (
                Decimal(open_interest) / Decimal(total_open_interest)
                if open_interest is not None and total_open_interest > 0
                else Decimal(0)
            )
            score = self.volume_weight * volume_component + self.open_interest_weight * open_interest_component
            breakdown.append(
                ScoreBreakdown(
                    contract_id=contract_id,
                    volume=volume,
                    open_interest=open_interest,
                    volume_component=volume_component,
                    open_interest_component=open_interest_component,
                    score=score,
                )
            )
        return tuple(breakdown)

    def front_contract(
        self, chain: ContractChain, observations: Sequence[RollObservation], as_of: date
    ) -> ContractId | None:
        listed = listed_contract_ids_at(chain, as_of)
        if not listed:
            return None
        breakdown = {entry.contract_id: entry for entry in self.score_breakdown(listed, observations, as_of)}
        return max(listed, key=lambda contract_id: breakdown[contract_id].score)
