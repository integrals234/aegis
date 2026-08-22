"""AEGIS-148 -- multiple-market validation must not silently select only
the best market."""

from __future__ import annotations

from decimal import Decimal
from pathlib import Path

import pytest
from research.strategy_replay import ReplayConfig
from validation.markets import configured_product_roots, run_multi_market_validation

pytestmark = pytest.mark.unit


def test_configured_product_roots_reads_the_canonical_config(repo_root: Path) -> None:
    roots = configured_product_roots(repo_root)
    # Verified from the canonical config, not assumed from memory.
    assert roots == ("EQX", "CLX", "SRX")


def test_every_configured_market_appears_in_the_report(repo_root: Path) -> None:
    roots = configured_product_roots(repo_root)
    config = ReplayConfig(zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1))
    report = run_multi_market_validation(roots, config, base_seed=100)

    assert len(report.markets) == len(roots)
    assert {m.product_root for m in report.markets} == set(roots)
    records = report.as_records()
    assert len(records) == len(roots)


def test_a_market_with_zero_round_trips_still_appears() -> None:
    # An entry_threshold high enough that nothing ever crosses it.
    config = ReplayConfig(zscore_window=20, entry_threshold=1000.0, exit_threshold=0.5, quantity_units=Decimal(1))
    report = run_multi_market_validation(("EQX",), config, base_seed=1)
    assert len(report.markets) == 1
    assert report.markets[0].result.round_trips == ()
    record = report.as_records()[0]
    assert record["round_trip_count"] == 0  # Present as zero, not omitted.


def test_markets_use_distinct_seeds_so_each_is_independently_reproducible() -> None:
    config = ReplayConfig(zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1))
    report = run_multi_market_validation(("EQX", "CLX"), config, base_seed=50)
    seeds = [m.seed for m in report.markets]
    assert len(set(seeds)) == len(seeds)
