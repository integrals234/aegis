"""Deterministic synthetic multi-day calendar-spread observation series,
shared internally by the M5 validation modules (private to this package --
not part of the public AEGIS-13x/14x/15x API surface any module exposes).

Every series here is a seeded, documented construction (a discrete
mean-reverting walk), never observed market data -- consistent with every
other synthetic dataset in this repository (docs/DATA_AND_RESEARCH_POLICY.md,
ADR-0025). It exists so validation modules that need many days of data
(walk-forward folds, bootstrap draws, stability grids) do not each invent
their own generator with its own accidental parameterization.
"""

from __future__ import annotations

import random
from datetime import date, timedelta
from decimal import Decimal

from futures.identifiers import ContractId
from research.calendar_spread import CalendarSpreadObservation

__all__ = ["make_synthetic_spread_series"]

_BASE_DAY = date(2026, 1, 1)


def make_synthetic_spread_series(
    product_root: str,
    *,
    num_days: int = 120,
    seed: int,
    base_spread: float = 50.0,
    spread_volatility: float = 8.0,
    mean_reversion: float = 0.15,
    drift: float = 0.0,
) -> tuple[CalendarSpreadObservation, ...]:
    """A seeded, deterministic mean-reverting spread series for
    ``product_root`` (must be one of ``configs/futures/products.yaml``'s
    families, e.g. ``"EQX"``, ``"CLX"``, ``"SRX"`` -- this function does not
    itself validate the name, a caller reading the canonical config does).
    Same ``seed`` -> byte-identical series; a different ``seed`` may differ,
    as validation.resampling's tests expect.
    """
    rng = random.Random(seed)
    near = ContractId(venue="SYNX", product_root=product_root, year=2026, month=3)
    far = ContractId(venue="SYNX", product_root=product_root, year=2026, month=6)

    near_price = Decimal(100_000)
    spread = base_spread
    observations = []
    for i in range(num_days):
        spread += mean_reversion * (base_spread - spread) + rng.gauss(0.0, spread_volatility) + drift
        far_price = near_price + Decimal(str(round(spread, 4)))
        observations.append(
            CalendarSpreadObservation(
                as_of=_BASE_DAY + timedelta(days=i),
                near_contract_id=near,
                far_contract_id=far,
                near_price=near_price,
                far_price=far_price,
                roll_policy_name="synthetic_validation_fixture",
                far_price_provenance=(
                    f"deterministic synthetic mean-reverting series, seed={seed}, "
                    "no real market data (docs/DATA_AND_RESEARCH_POLICY.md)"
                ),
                contract_steps=1,
                far_price_observed=False,
            )
        )
    return tuple(observations)
