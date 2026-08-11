"""Columnar interchange over `futures_bar.v1` (AEGIS-230).

Normalized records -> Arrow -> Parquet -> Arrow/readback -> schema/version
validation -> DuckDB query over the same Parquet, all over the one
`futures_bar.v1` schema `python/futures/schema.py` defines -- no second,
competing schema for the columnar path. Schema version travels in the
Arrow/Parquet file's own key-value metadata (`pyarrow` carries Arrow schema
metadata into the Parquet footer automatically), and `read_parquet` refuses
to interpret a file whose declared schema name/version it does not
recognize -- the same "reject, never reinterpret" discipline every other
AEGIS schema in this milestone follows.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any, Final

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

from futures.schema import NORMALIZED_COLUMNS, SCHEMA_NAME, SCHEMA_VERSION

__all__ = [
    "ColumnarError",
    "query_duckdb",
    "read_parquet",
    "table_to_records",
    "to_arrow_table",
    "write_parquet",
]

_METADATA_NAME_KEY: Final[bytes] = b"aegis_schema_name"
_METADATA_VERSION_KEY: Final[bytes] = b"aegis_schema_version"

_ARROW_TYPES: Final[dict[str, pa.DataType]] = {
    "schema_version": pa.int64(),
    "venue": pa.string(),
    "product_root": pa.string(),
    "contract_symbol": pa.string(),
    "event_time_ns": pa.int64(),
    "open_ticks": pa.int64(),
    "high_ticks": pa.int64(),
    "low_ticks": pa.int64(),
    "close_ticks": pa.int64(),
    "volume": pa.int64(),
    "open_interest": pa.int64(),
    "settlement_price_ticks": pa.int64(),
    "source_sequence": pa.int64(),
    "record_index": pa.int64(),
}


class ColumnarError(ValueError):
    """A Parquet file could not be interpreted: missing or unsupported
    AEGIS schema metadata. Raised rather than guessed -- reading an unknown
    version under today's field meanings is exactly what every other AEGIS
    schema in this milestone refuses to do."""


def to_arrow_table(records: Sequence[Mapping[str, Any]]) -> pa.Table:
    """Normalized records -> an Arrow table with fixed column order and
    embedded schema identity. Every record must already carry the same
    ``schema_version`` -- this is a writer of one schema, not a translator
    between versions."""
    columns: dict[str, list[Any]] = {name: [] for name in NORMALIZED_COLUMNS}
    for record in records:
        for name in NORMALIZED_COLUMNS:
            columns[name].append(record[name])
    arrays = [pa.array(columns[name], type=_ARROW_TYPES[name]) for name in NORMALIZED_COLUMNS]
    table = pa.Table.from_arrays(arrays, names=list(NORMALIZED_COLUMNS))
    return table.replace_schema_metadata(
        {
            _METADATA_NAME_KEY: SCHEMA_NAME.encode("utf-8"),
            _METADATA_VERSION_KEY: str(SCHEMA_VERSION).encode("utf-8"),
        }
    )


def write_parquet(table: pa.Table, path: Path) -> None:
    pq.write_table(table, path)


def read_parquet(path: Path) -> pa.Table:
    """Read back a Parquet file written by :func:`write_parquet`, refusing
    one whose declared schema name/version this build does not recognize."""
    table = pq.read_table(path)
    metadata = table.schema.metadata or {}
    name = metadata.get(_METADATA_NAME_KEY)
    version = metadata.get(_METADATA_VERSION_KEY)
    if name is None or version is None:
        raise ColumnarError(
            f"{path}: no AEGIS schema metadata found; refusing to interpret an "
            "unidentified Parquet file as futures_bar data"
        )
    if name.decode("utf-8") != SCHEMA_NAME or version.decode("utf-8") != str(SCHEMA_VERSION):
        raise ColumnarError(
            f"{path}: schema {name.decode('utf-8')!r} v{version.decode('utf-8')!r} is not "
            f"supported by this build (this build understands {SCHEMA_NAME!r} v{SCHEMA_VERSION})"
        )
    return table


def table_to_records(table: pa.Table) -> list[dict[str, Any]]:
    """Inverse of :func:`to_arrow_table`. Column order in the returned dicts
    follows the table's own (already-fixed) schema, never a re-derived one."""
    records: list[dict[str, Any]] = table.to_pylist()
    return records


def query_duckdb(path: Path, sql: str, view_name: str = "bars") -> list[tuple[Any, ...]]:
    """Run ``sql`` (referencing ``view_name``) against the Parquet file at
    ``path`` via DuckDB, over the same file -- no separate DuckDB-native
    copy of the data."""
    escaped_path = str(path).replace("'", "''")
    connection = duckdb.connect(database=":memory:")
    try:
        connection.execute(f"CREATE VIEW {view_name} AS SELECT * FROM read_parquet('{escaped_path}')")
        return connection.execute(sql).fetchall()
    finally:
        connection.close()
