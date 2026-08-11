"""AEGIS-230 -- columnar interchange round trip over `futures_bar.v1`.

normalized futures records -> Arrow -> Parquet -> Arrow/readback -> schema
version validation -> DuckDB query over the same Parquet, all real
production components (`futures.ingest`, `futures.columnar`), driven by the
three committed bar fixtures -- the same interface AEGIS-026's counterpart
integration test uses, one schema throughout.
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import pytest
from futures.columnar import (
    ColumnarError,
    query_duckdb,
    read_parquet,
    table_to_records,
    to_arrow_table,
    write_parquet,
)
from futures.ingest import ingest
from futures.instruments import DEFAULT_CATALOG_PATH, load_catalog
from futures.schema import NORMALIZED_COLUMNS, SCHEMA_VERSION

pytestmark = pytest.mark.integration

ROOT = Path(__file__).resolve().parents[2]
BAR_PATHS = (
    "data_samples/futures/bars/eqx.csv",
    "data_samples/futures/bars/clx.jsonl",
    "data_samples/futures/bars/srx.csv",
)


def _ingested_records() -> tuple[dict, ...]:
    catalog = load_catalog(ROOT, DEFAULT_CATALOG_PATH)
    result = ingest(ROOT, BAR_PATHS, catalog)
    assert not result.rejections
    return result.records


def test_round_trip_preserves_row_count_and_values() -> None:
    records = _ingested_records()
    table = to_arrow_table(records)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        write_parquet(table, path)
        read_back = read_parquet(path)
        round_tripped = table_to_records(read_back)

    assert len(round_tripped) == len(records)
    assert round_tripped == [dict(r) for r in records]


def test_round_trip_preserves_exact_integer_tick_prices() -> None:
    records = _ingested_records()
    table = to_arrow_table(records)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        write_parquet(table, path)
        round_tripped = table_to_records(read_parquet(path))

    for original, restored in zip(records, round_tripped, strict=True):
        for field in ("open_ticks", "high_ticks", "low_ticks", "close_ticks"):
            assert restored[field] == original[field]
            assert isinstance(restored[field], int)


def test_round_trip_preserves_event_time_ns_exactly() -> None:
    records = _ingested_records()
    table = to_arrow_table(records)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        write_parquet(table, path)
        round_tripped = table_to_records(read_parquet(path))

    for original, restored in zip(records, round_tripped, strict=True):
        assert restored["event_time_ns"] == original["event_time_ns"]


def test_round_trip_preserves_record_index_and_contract_identity() -> None:
    records = _ingested_records()
    table = to_arrow_table(records)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        write_parquet(table, path)
        round_tripped = table_to_records(read_parquet(path))

    assert [r["record_index"] for r in round_tripped] == [r["record_index"] for r in records]
    assert [r["contract_symbol"] for r in round_tripped] == [r["contract_symbol"] for r in records]


def test_round_trip_preserves_nullability_of_volume_and_open_interest() -> None:
    records = list(_ingested_records())
    # Force at least one null in each nullable field.
    records[0] = {**records[0], "volume": None}
    records[1] = {**records[1], "open_interest": None}
    table = to_arrow_table(records)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        write_parquet(table, path)
        round_tripped = table_to_records(read_parquet(path))

    assert round_tripped[0]["volume"] is None
    assert round_tripped[1]["open_interest"] is None
    assert round_tripped[2]["volume"] is not None


def test_column_order_is_deterministic() -> None:
    records = _ingested_records()
    table = to_arrow_table(records)
    assert table.column_names == list(NORMALIZED_COLUMNS)


def test_schema_version_travels_in_parquet_metadata() -> None:
    records = _ingested_records()
    table = to_arrow_table(records)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        write_parquet(table, path)
        read_back = read_parquet(path)
    metadata = read_back.schema.metadata
    assert metadata[b"aegis_schema_version"] == str(SCHEMA_VERSION).encode("utf-8")


def test_unknown_schema_version_rejected() -> None:
    import pyarrow.parquet as pq

    records = _ingested_records()
    table = to_arrow_table(records)
    tampered = table.replace_schema_metadata(
        {b"aegis_schema_name": b"futures_bar", b"aegis_schema_version": b"99"}
    )
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        pq.write_table(tampered, path)
        with pytest.raises(ColumnarError, match="not supported"):
            read_parquet(path)


def test_missing_schema_metadata_rejected() -> None:
    import pyarrow.parquet as pq

    records = _ingested_records()
    table = to_arrow_table(records)
    stripped = table.replace_schema_metadata({})
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        pq.write_table(stripped, path)
        with pytest.raises(ColumnarError, match="no AEGIS schema metadata"):
            read_parquet(path)


def test_duckdb_query_over_the_same_parquet_file() -> None:
    records = _ingested_records()
    table = to_arrow_table(records)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        write_parquet(table, path)
        rows = query_duckdb(
            path, "SELECT product_root, count(*) FROM bars GROUP BY product_root ORDER BY product_root"
        )
    assert rows == [("CLX", 6), ("EQX", 6), ("SRX", 6)]


def test_duckdb_row_count_matches_arrow_row_count() -> None:
    records = _ingested_records()
    table = to_arrow_table(records)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        write_parquet(table, path)
        rows = query_duckdb(path, "SELECT count(*) FROM bars")
    assert rows[0][0] == len(records)
