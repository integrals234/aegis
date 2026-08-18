"""Calendar-spread stationarity testing (AEGIS-079; ADR-0026).

One documented test, not several competing ones: the unaugmented
Dickey-Fuller test for a unit root, against a constant with no trend, run
over a :class:`~research.calendar_spread.CalendarSpreadObservation`
sequence's own ``spread`` series.

No numpy/scipy/statsmodels dependency -- none is pinned in
``requirements/requirements.lock``, and adding one is out of this batch's
scope. The regression here has exactly two parameters (a constant and the
lagged level), solved by the closed-form ordinary-least-squares normal
equations -- the same "obviously correct against the textbook definition"
discipline ADR-0022 established for the offline statistics reference.

# What a result here does, and does not, mean

This module answers one narrow statistical question: over the SUPPLIED
sample, does the lagged-level regression coefficient differ from zero in the
direction that indicates mean reversion, at the stated significance level?
Rejecting the null (unit root) is evidence the sample behaved as though
mean-reverting. **It is not a claim that the spread will remain stationary in
the future, a claim about live or real markets, or a claim about the
underlying data-generating process.** `docs/CV_CLAIMS_POLICY.md` and
`docs/DATA_AND_RESEARCH_POLICY.md` govern what may be claimed from research
output; this module reports the test's own result and nothing beyond it.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from decimal import Decimal
from enum import StrEnum
from typing import Final

from research.calendar_spread import CalendarSpreadObservation

__all__ = [
    "MIN_OBSERVATIONS",
    "InsufficientSample",
    "StationarityClassification",
    "StationarityTestResult",
    "test_spread_stationarity",
]


class InsufficientSample(ValueError):
    """Fewer than :data:`MIN_OBSERVATIONS` spread values were supplied, or
    the lagged series has zero variance -- the regression's degrees of
    freedom would be too thin, or undefined, to report a result honestly."""


# A regression with 2 parameters needs at least a handful of residual
# degrees of freedom to be worth reporting; 8 raw observations gives 7
# differenced rows and 5 residual degrees of freedom.
MIN_OBSERVATIONS: Final[int] = 8


class StationarityClassification(StrEnum):
    STATIONARY = "stationary"  # Null (unit root) rejected at the stated level.
    NON_STATIONARY = "non_stationary"  # Null not rejected.


# MacKinnon (1994) asymptotic critical values for the Dickey-Fuller
# regression with a constant and no trend. Fixed and documented, not
# adjusted for finite-sample size -- stated explicitly as a caveat rather
# than silently approximated.
_CRITICAL_VALUES: Final[dict[str, Decimal]] = {
    "1%": Decimal("-3.43"),
    "5%": Decimal("-2.86"),
    "10%": Decimal("-2.57"),
}


@dataclass(frozen=True, slots=True)
class StationarityTestResult:
    test_name: str
    null_hypothesis: str
    alternative_hypothesis: str
    sample_size: int
    regression_intercept: Decimal
    regression_slope: Decimal  # Coefficient on the lagged level.
    test_statistic: Decimal
    significance_level: str
    critical_value: Decimal
    classification: StationarityClassification
    assumptions: tuple[str, ...]
    caveats: tuple[str, ...]


def test_spread_stationarity(
    observations: Sequence[CalendarSpreadObservation], significance_level: str = "5%"
) -> StationarityTestResult:
    """AEGIS-079. Raises :class:`InsufficientSample` rather than silently
    reporting a result the sample cannot honestly support."""
    if significance_level not in _CRITICAL_VALUES:
        raise ValueError(
            f"significance_level must be one of {sorted(_CRITICAL_VALUES)}, got "
            f"{significance_level!r}"
        )

    spreads = [observation.spread for observation in observations]
    n = len(spreads)
    if n < MIN_OBSERVATIONS:
        raise InsufficientSample(f"need at least {MIN_OBSERVATIONS} observations, got {n}")

    # Delta y_t = alpha + beta * y_{t-1} + eps_t.
    y_lag = spreads[:-1]
    delta_y = [current - previous for previous, current in zip(y_lag, spreads[1:], strict=True)]
    m = len(delta_y)

    mean_lag = sum(y_lag, start=Decimal(0)) / m
    mean_delta = sum(delta_y, start=Decimal(0)) / m
    s_xy = sum(
        ((x - mean_lag) * (dy - mean_delta) for x, dy in zip(y_lag, delta_y, strict=True)),
        start=Decimal(0),
    )
    s_xx = sum(((x - mean_lag) ** 2 for x in y_lag), start=Decimal(0))
    if s_xx == 0:
        raise InsufficientSample("the lagged spread series has zero variance")

    beta = s_xy / s_xx
    alpha = mean_delta - beta * mean_lag

    residuals = [dy - (alpha + beta * x) for x, dy in zip(y_lag, delta_y, strict=True)]
    dof = m - 2  # Two estimated parameters (alpha, beta).
    if dof <= 0:
        raise InsufficientSample(f"need more than 2 regression rows after differencing, got {m}")
    ss_res = sum((residual**2 for residual in residuals), start=Decimal(0))
    sigma_squared = ss_res / dof
    se_beta = (sigma_squared / s_xx).sqrt()

    test_statistic = beta / se_beta if se_beta != 0 else Decimal(0)
    critical_value = _CRITICAL_VALUES[significance_level]
    classification = (
        StationarityClassification.STATIONARY
        if test_statistic < critical_value
        else StationarityClassification.NON_STATIONARY
    )

    return StationarityTestResult(
        test_name="Dickey-Fuller (unaugmented, constant, no trend)",
        null_hypothesis="the spread series has a unit root (is non-stationary / a random walk with drift)",
        alternative_hypothesis="the spread series is stationary (mean-reverting) around a constant",
        sample_size=n,
        regression_intercept=alpha,
        regression_slope=beta,
        test_statistic=test_statistic,
        significance_level=significance_level,
        critical_value=critical_value,
        classification=classification,
        assumptions=(
            "no augmentation lags: residuals are assumed serially uncorrelated after "
            "one difference",
            "critical values are MacKinnon (1994) asymptotic values for a constant, "
            "no-trend regression, not adjusted for this sample's finite size",
            "the test statistic's distribution is the Dickey-Fuller distribution, not "
            "the standard normal or Student's t",
        ),
        caveats=(
            "this is a statistical test result over the supplied sample only, not a "
            "claim that the spread will remain stationary in the future",
            "the underlying near/far price data is synthetic/constructed (ADR-0025); "
            "no claim is made about real markets",
            "a classification of 'stationary' does not imply any particular hedge "
            "ratio, holding period, or trading outcome",
        ),
    )
