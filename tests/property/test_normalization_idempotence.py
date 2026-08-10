"""M2 slice 4 -- normalization determinism/idempotence over arbitrary valid
input, not just the handful of examples in test_futures_ingest.py.

For any set of well-formed rows, ingesting the same bytes twice must produce
identical normalized records (a pure function of input bytes, input path set
and ingestion config -- M2 plan of record section 7), and every produced
record must validate against the committed `futures_bar.v1` schema.
"""

from __future__ import annotations

import csv
import io
import tempfile
from decimal import Decimal
from pathlib import Path

import pytest
from futures.ingest import ingest
from futures.instruments import Product, ProductCatalog
from futures.schema import SCHEMA_NAME, build_registry
from hypothesis import given, settings
from hypothesis import strategies as st

pytestmark = pytest.mark.property

ROOT = Path(__file__).resolve().parents[2]
TICK_SIZE = Decimal("0.25")

tick_counts = st.integers(min_value=-40_000, max_value=40_000)
event_times = st.integers(min_value=0, max_value=2_000_000_000_000_000_000)
volumes = st.one_of(st.none(), st.integers(min_value=0, max_value=10_000_000))


def _catalog() -> ProductCatalog:
    product = Product(
        venue="SYNX",
        product_root="EQX",
        description="property-test product",
        tick_size=TICK_SIZE,
        lot_size=1,
        multiplier=Decimal("50"),
        currency="USD",
        timezone="America/Chicago",
        session_template="synx_equity_index_rth",
    )
    return ProductCatalog([product])


rows = st.builds(
    dict,
    open_ticks=tick_counts,
    high_ticks=tick_counts,
    low_ticks=tick_counts,
    close_ticks=tick_counts,
    event_time_ns=event_times,
    volume=volumes,
    open_interest=volumes,
)


def _write_csv(path: Path, entries: list[dict[str, object]]) -> None:
    buffer = io.StringIO()
    columns = [
        "contract_symbol",
        "event_time_ns",
        "open",
        "high",
        "low",
        "close",
        "settlement_price",
        "volume",
        "open_interest",
        "source_sequence",
    ]
    writer = csv.DictWriter(buffer, fieldnames=columns, lineterminator="\n")
    writer.writeheader()
    for index, entry in enumerate(entries):
        writer.writerow(
            {
                "contract_symbol": "SYNX:EQX:2026H",
                "event_time_ns": entry["event_time_ns"],
                "open": str(entry["open_ticks"] * TICK_SIZE),
                "high": str(entry["high_ticks"] * TICK_SIZE),
                "low": str(entry["low_ticks"] * TICK_SIZE),
                "close": str(entry["close_ticks"] * TICK_SIZE),
                "settlement_price": "",
                "volume": "" if entry["volume"] is None else entry["volume"],
                "open_interest": "" if entry["open_interest"] is None else entry["open_interest"],
                "source_sequence": index,
            }
        )
    path.write_text(buffer.getvalue(), encoding="utf-8")


@given(entries=st.lists(rows, min_size=1, max_size=8))
@settings(max_examples=100)
def test_repeated_ingestion_is_byte_identical(entries: list[dict[str, object]]) -> None:
    catalog = _catalog()
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "rows.csv"
        _write_csv(path, entries)
        first = ingest(ROOT, [path], catalog)
        second = ingest(ROOT, [path], catalog)
    assert first.records == second.records
    assert first.rejections == second.rejections
    assert first.out_of_order == second.out_of_order


@given(entries=st.lists(rows, min_size=1, max_size=8))
@settings(max_examples=100)
def test_every_normalized_record_validates(entries: list[dict[str, object]]) -> None:
    catalog = _catalog()
    registry = build_registry(ROOT)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "rows.csv"
        _write_csv(path, entries)
        result = ingest(ROOT, [path], catalog)
    assert not result.rejections
    for record in result.records:
        registry.validate(SCHEMA_NAME, record)


@given(entries=st.lists(rows, min_size=1, max_size=8))
@settings(max_examples=100)
def test_record_index_is_contiguous_and_ticks_are_exact(entries: list[dict[str, object]]) -> None:
    catalog = _catalog()
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "rows.csv"
        _write_csv(path, entries)
        result = ingest(ROOT, [path], catalog)
    indexes = [record["record_index"] for record in result.records]
    assert indexes == list(range(len(entries)))
    for record, entry in zip(result.records, entries, strict=True):
        assert record["open_ticks"] == entry["open_ticks"]
        assert record["high_ticks"] == entry["high_ticks"]
        assert record["low_ticks"] == entry["low_ticks"]
        assert record["close_ticks"] == entry["close_ticks"]
