"""AEGIS-149 -- every configured regime must appear in the report,
including one with zero trades."""

from __future__ import annotations

from decimal import Decimal
from pathlib import Path

import pytest
from research.strategy_replay import ReplayConfig
from validation._fixtures import make_synthetic_spread_series
from validation.regimes import load_regime_definitions, run_regime_evaluation

pytestmark = pytest.mark.unit


def test_loads_regimes_from_the_canonical_config(repo_root: Path) -> None:
    regimes = load_regime_definitions(repo_root)
    assert len(regimes) >= 1
    assert all(r.start <= r.end for r in regimes)


def test_every_configured_regime_appears_including_zero_trade_ones(repo_root: Path) -> None:
    regimes = load_regime_definitions(repo_root)
    observations = make_synthetic_spread_series("EQX", seed=9)
    config = ReplayConfig(zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1))

    report = run_regime_evaluation(observations, regimes, config)

    assert len(report.regimes) == len(regimes)
    assert {r.name for r in report.regimes} == {r.name for r in regimes}
    # At least one configured regime (outside_series) covers zero observations.
    zero_observation_regimes = [r for r in report.regimes if r.observation_count == 0]
    assert zero_observation_regimes
    for r in zero_observation_regimes:
        assert r.result.round_trips == ()  # Present as zero, not omitted from the report.


def test_regime_containment_is_correct_and_reproducible() -> None:
    from datetime import date

    from validation.regimes import RegimeDefinition

    regime = RegimeDefinition(name="test", start=date(2026, 1, 5), end=date(2026, 1, 10))
    assert not regime.contains(date(2026, 1, 4))
    assert regime.contains(date(2026, 1, 5))
    assert regime.contains(date(2026, 1, 10))
    assert not regime.contains(date(2026, 1, 11))


def test_regime_definition_rejects_start_after_end() -> None:
    from datetime import date

    from validation.regimes import RegimeDefinition

    with pytest.raises(ValueError, match="after end"):
        RegimeDefinition(name="bad", start=date(2026, 1, 10), end=date(2026, 1, 5))
