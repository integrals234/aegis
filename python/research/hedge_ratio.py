"""Calendar-spread hedge-ratio estimation (AEGIS-078; ADR-0026).

Two documented forms, both over the near/far *price* series a
:class:`~research.calendar_spread.CalendarSpreadObservation` sequence already
carries:

* **static** -- a single OLS slope (``far ~ hedge_ratio * near``) over the
  whole supplied window, via the covariance/variance identity computed
  directly from its two-pass textbook definition -- the same discipline
  ``python/common/offline_stats.py`` uses for AEGIS-107's independent
  reference (ADR-0022), not an updating recursion.
* **rolling** -- one hedge ratio per observation, estimated only from the
  ``window`` observations strictly *before* it (ADR-0026's leakage rule,
  matching ``cpp/statistics/rolling_zscore.hpp``'s own "prior window"
  convention) -- it never includes the observation it will then be used
  against.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal

from research.calendar_spread import CalendarSpreadObservation

__all__ = [
    "InsufficientObservations",
    "RollingHedgeRatioObservation",
    "rolling_hedge_ratio",
    "static_hedge_ratio",
]


class InsufficientObservations(ValueError):
    """Fewer than two observations, or a near-price series with zero
    variance, were supplied -- a slope is not defined against a constant or
    single-point series, and this is never silently substituted."""


def _slope(near: Sequence[Decimal], far: Sequence[Decimal]) -> Decimal:
    count = len(near)
    if count < 2:
        raise InsufficientObservations(f"need at least 2 observations, got {count}")
    mean_near = sum(near, start=Decimal(0)) / count
    mean_far = sum(far, start=Decimal(0)) / count
    covariance = sum(
        ((x - mean_near) * (y - mean_far) for x, y in zip(near, far, strict=True)),
        start=Decimal(0),
    )
    variance = sum(((x - mean_near) ** 2 for x in near), start=Decimal(0))
    if variance == 0:
        raise InsufficientObservations("near-price series has zero variance over this window")
    return covariance / variance


def static_hedge_ratio(observations: Sequence[CalendarSpreadObservation]) -> Decimal:
    """AEGIS-078: one hedge ratio over the whole supplied window."""
    return _slope(
        [observation.near_price for observation in observations],
        [observation.far_price for observation in observations],
    )


@dataclass(frozen=True, slots=True)
class RollingHedgeRatioObservation:
    """``hedge_ratio`` is ``None`` for the first ``window`` observations --
    a documented edge case (not enough prior history yet), never a silently
    substituted value."""

    as_of: date
    hedge_ratio: Decimal | None


def rolling_hedge_ratio(
    observations: Sequence[CalendarSpreadObservation], window: int
) -> tuple[RollingHedgeRatioObservation, ...]:
    """AEGIS-078 rolling form. Observation ``i``'s ratio uses observations
    ``[i - window, i)`` -- strictly prior, never including ``i`` itself
    (leakage-free, ADR-0026).
    """
    if window < 2:
        raise ValueError(f"window must be >= 2, got {window}")

    results: list[RollingHedgeRatioObservation] = []
    for index, observation in enumerate(observations):
        if index < window:
            results.append(RollingHedgeRatioObservation(as_of=observation.as_of, hedge_ratio=None))
            continue
        prior = observations[index - window : index]
        try:
            ratio = _slope(
                [item.near_price for item in prior], [item.far_price for item in prior]
            )
        except InsufficientObservations:
            ratio = None
        results.append(RollingHedgeRatioObservation(as_of=observation.as_of, hedge_ratio=ratio))
    return tuple(results)
