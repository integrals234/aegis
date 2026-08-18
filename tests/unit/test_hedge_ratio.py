"""AEGIS-078 -- hedge-ratio estimation, static and rolling (ADR-0026)."""

from __future__ import annotations

import csv
import datetime
from datetime import date, timedelta
from decimal import Decimal
from pathlib import Path

import pytest
from futures.chain import ContractChain
from futures.identifiers import ContractId
from futures.roll.fixed_days import FixedDaysPolicy
from futures.series import PriceObservation
from make_futures_fixtures import load_family
from research.calendar_spread import (
    CalendarSpreadObservation,
    ConstructedBasisRule,
    build_calendar_spread_observations,
)
from research.hedge_ratio import (
    InsufficientObservations,
    rolling_hedge_ratio,
    static_hedge_ratio,
)

pytestmark = pytest.mark.unit

NEAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
FAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)
BASE_DAY = date(2026, 1, 1)


def _observations(near_prices: list[Decimal], far_prices: list[Decimal]) -> list[CalendarSpreadObservation]:
    return [
        CalendarSpreadObservation(
            as_of=BASE_DAY + timedelta(days=i),
            near_contract_id=NEAR,
            far_contract_id=FAR,
            near_price=near,
            far_price=far,
            roll_policy_name="FixedDaysPolicy",
            far_price_provenance="test fixture",
            contract_steps=1,
        )
        for i, (near, far) in enumerate(zip(near_prices, far_prices, strict=True))
    ]


def test_static_hedge_ratio_recovers_an_exact_linear_relationship() -> None:
    # far = 2 * near + 5 exactly: the OLS slope must recover 2 exactly.
    near = [Decimal(x) for x in (10, 20, 30, 40)]
    far = [2 * x + 5 for x in near]
    observations = _observations(near, far)
    assert static_hedge_ratio(observations) == Decimal(2)


def test_static_hedge_ratio_needs_at_least_two_observations() -> None:
    observations = _observations([Decimal(10)], [Decimal(20)])
    with pytest.raises(InsufficientObservations, match="at least 2"):
        static_hedge_ratio(observations)


def test_static_hedge_ratio_rejects_zero_variance_near_series() -> None:
    observations = _observations([Decimal(10), Decimal(10)], [Decimal(20), Decimal(25)])
    with pytest.raises(InsufficientObservations, match="zero variance"):
        static_hedge_ratio(observations)


def test_rolling_hedge_ratio_uses_only_prior_observations_never_the_current_one() -> None:
    # far = 3 * near for indices 0..3, then the relationship changes sharply
    # at index 4 (far = 100 * near). If index 4's own value leaked into its
    # own window, its ratio would be pulled toward 100; leakage-free, it must
    # still reflect only the prior (ratio == 3) window.
    near = [Decimal(x) for x in (1, 2, 3, 4, 5)]
    far = [3 * near[0], 3 * near[1], 3 * near[2], 3 * near[3], 100 * near[4]]
    observations = _observations(near, far)

    results = rolling_hedge_ratio(observations, window=4)

    assert [r.hedge_ratio for r in results[:4]] == [None, None, None, None]
    assert results[4].hedge_ratio == Decimal(3)  # Not 100 -- index 4 never entered its own window.


def test_rolling_hedge_ratio_reports_none_when_the_prior_window_has_zero_variance() -> None:
    near = [Decimal(10), Decimal(10), Decimal(10)]
    far = [Decimal(20), Decimal(20), Decimal(30)]
    observations = _observations(near, far)
    results = rolling_hedge_ratio(observations, window=2)
    assert results[2].hedge_ratio is None


def test_rolling_hedge_ratio_rejects_window_below_two() -> None:
    with pytest.raises(ValueError, match="window must be >= 2"):
        rolling_hedge_ratio(_observations([Decimal(1)], [Decimal(1)]), window=1)


# --- Historical validation (AEGIS-078's "synthetic AND historical") ---------
#
# The tests above are synthetic: hand-built Decimal series with analytically
# known slopes. These run the same estimators over the committed EQX
# settlement bars in data_samples/futures/bars/eqx.csv -- real committed
# historical input, loaded through the same path the demo generator uses.


def _eqx_spread_observations(repo_root: Path) -> tuple[CalendarSpreadObservation, ...]:
    contracts_path = repo_root / "data_samples/futures/eqx.json"
    bars_path = repo_root / "data_samples/futures/bars/eqx.csv"
    venue, product_root, contracts = load_family(contracts_path)

    chain = ContractChain(venue, product_root)
    for contract in contracts:
        chain.add(contract)

    prices: list[PriceObservation] = []
    as_of_dates: list[date] = []
    with bars_path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            contract_id = next(
                c.contract_id for c in contracts if c.contract_id.canonical == row["contract_symbol"]
            )
            session_date = datetime.datetime.fromtimestamp(
                int(row["event_time_ns"]) / 1e9, tz=datetime.UTC
            ).date()
            prices.append(
                PriceObservation(
                    contract_id=contract_id,
                    session_date=session_date,
                    price=Decimal(row["settlement_price"]),
                )
            )
            as_of_dates.append(session_date)

    return build_calendar_spread_observations(
        chain=chain,
        policy=FixedDaysPolicy(days_before_expiry=0),
        roll_observations=(),
        near_prices=prices,
        as_of_dates=as_of_dates,
        basis_rule=ConstructedBasisRule(
            basis_units_by_index=(
                Decimal("0.50"),
                Decimal("0.55"),
                Decimal("0.60"),
                Decimal("0.65"),
                Decimal("2.50"),
                Decimal("0.70"),
            ),
            description="constructed far leg (ADR-0025); NOT observed market data",
        ),
    )


def _slope_from_definition(xs: list[Decimal], ys: list[Decimal]) -> Decimal:
    """Two-pass OLS slope straight from the textbook definition, computed in
    the test so the expectation below is derived independently of the module
    under test."""
    n = len(xs)
    mean_x = sum(xs, start=Decimal(0)) / n
    mean_y = sum(ys, start=Decimal(0)) / n
    cov = sum(((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys, strict=True)), start=Decimal(0))
    var = sum(((x - mean_x) ** 2 for x in xs), start=Decimal(0))
    return cov / var


def test_static_hedge_ratio_over_committed_historical_bars(repo_root: Path) -> None:
    """Historical half of AEGIS-078: the estimator run over the committed EQX
    settlement bars.

    The expectation is derived independently rather than copied from the
    output. The committed EQX chain has bars for one contract only, so the far
    leg is the documented additive construction ``far = near + basis[i]``
    (ADR-0025). For an additive far leg the far-on-near slope is exactly
    ``1 + cov(near, basis) / var(near)`` -- and the estimator never sees
    ``basis``, only ``near`` and ``far``, so recovering that value is a real
    check rather than a restatement.
    """
    observations = _eqx_spread_observations(repo_root)
    assert len(observations) == 6
    assert all(not o.far_price_observed for o in observations)  # Construction path, disclosed.

    near = [o.near_price for o in observations]
    far = [o.far_price for o in observations]
    basis = [f - n for n, f in zip(near, far, strict=True)]
    assert basis != [basis[0]] * len(basis)  # Non-constant: the slope is genuinely not 1.

    expected = Decimal(1) + _slope_from_definition(near, basis)
    actual = static_hedge_ratio(observations)
    assert actual == pytest.approx(expected, abs=Decimal("1e-24"))


def test_rolling_hedge_ratio_over_committed_historical_bars_is_leakage_free(
    repo_root: Path,
) -> None:
    """Same committed historical input, rolling form: the first ``window``
    observations report ``None`` (insufficient prior history), and every later
    estimate matches the independently-derived slope over its own strictly
    prior window -- never a window including the observation being scored."""
    window = 3
    observations = _eqx_spread_observations(repo_root)
    results = rolling_hedge_ratio(observations, window=window)

    assert [r.as_of for r in results] == [o.as_of for o in observations]
    assert all(r.hedge_ratio is None for r in results[:window])

    near = [o.near_price for o in observations]
    basis = [o.far_price - o.near_price for o in observations]
    for index in range(window, len(observations)):
        prior_near = near[index - window : index]
        prior_basis = basis[index - window : index]
        expected = Decimal(1) + _slope_from_definition(prior_near, prior_basis)
        actual = results[index].hedge_ratio
        assert actual is not None
        assert actual == pytest.approx(expected, abs=Decimal("1e-24")), index
