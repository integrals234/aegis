"""AEGIS-026 — at least three futures product families load through ONE
normalized interface.

Integration rather than unit: this drives the real, committed artifacts
(``configs/futures/products.yaml``, ``data_samples/futures/bars/*``) through
the real production components (``futures.instruments.load_catalog``,
``futures.ingest.ingest``) end to end, exactly as
``tools/generate_futures_evidence.py``'s slice-4 counterpart will for
evidence. A unit test proving ``ingest()`` works on a synthetic fixture does
not prove the *committed* three-family data actually loads; this does.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from futures.ingest import ingest
from futures.instruments import DEFAULT_CATALOG_PATH, load_catalog
from futures.schema import SCHEMA_NAME, build_registry

pytestmark = pytest.mark.integration

ROOT = Path(__file__).resolve().parents[2]
BAR_PATHS = (
    "data_samples/futures/bars/eqx.csv",
    "data_samples/futures/bars/clx.jsonl",
    "data_samples/futures/bars/srx.csv",
)


def test_three_committed_families_load_through_one_interface() -> None:
    catalog = load_catalog(ROOT, DEFAULT_CATALOG_PATH)
    result = ingest(ROOT, BAR_PATHS, catalog)

    assert not result.rejections
    assert not result.out_of_order
    assert len(result.records) == 18  # 3 families x 6 bars each

    product_roots = {record["product_root"] for record in result.records}
    assert product_roots == {"EQX", "CLX", "SRX"}

    contract_symbols = {record["contract_symbol"] for record in result.records}
    assert len(contract_symbols) == 3  # one contract per family


def test_every_record_validates_against_the_committed_schema() -> None:
    catalog = load_catalog(ROOT, DEFAULT_CATALOG_PATH)
    registry = build_registry(ROOT)
    result = ingest(ROOT, BAR_PATHS, catalog)
    for record in result.records:
        registry.validate(SCHEMA_NAME, record)


def test_record_index_is_contiguous_across_all_three_families() -> None:
    catalog = load_catalog(ROOT, DEFAULT_CATALOG_PATH)
    result = ingest(ROOT, BAR_PATHS, catalog)
    indexes = [record["record_index"] for record in result.records]
    assert indexes == list(range(len(indexes)))


def test_no_floating_point_prices_on_the_canonical_path() -> None:
    catalog = load_catalog(ROOT, DEFAULT_CATALOG_PATH)
    result = ingest(ROOT, BAR_PATHS, catalog)
    for record in result.records:
        for field in ("open_ticks", "high_ticks", "low_ticks", "close_ticks"):
            assert isinstance(record[field], int)
