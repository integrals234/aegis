#!/usr/bin/env python3
"""Generate AEGIS-230 evidence: the real Arrow/Parquet/DuckDB round trip.

Drives the real production path -- futures.ingest.ingest -> futures.columnar
-- over the three committed bar fixtures and records every round-trip
invariant checked, so the evidence proves what
tests/integration/test_columnar_roundtrip.py proves, not a parallel claim.

Regenerate with: python3 tools/generate_columnar_evidence.py
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from futures.columnar import query_duckdb, read_parquet, table_to_records, to_arrow_table, write_parquet
from futures.ingest import ingest
from futures.instruments import DEFAULT_CATALOG_PATH, load_catalog
from futures.schema import NORMALIZED_COLUMNS, SCHEMA_VERSION

BAR_PATHS = (
    "data_samples/futures/bars/eqx.csv",
    "data_samples/futures/bars/clx.jsonl",
    "data_samples/futures/bars/srx.csv",
)


def _git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else ""


def _provenance() -> dict[str, Any]:
    return {
        "generated_on": datetime.now(UTC).strftime("%Y-%m-%d"),
        "repository_commit": _git("rev-parse", "HEAD"),
        "dirty": bool(_git("status", "--porcelain")),
    }


def main(argv: list[str] | None = None) -> int:
    del argv
    catalog = load_catalog(ROOT, DEFAULT_CATALOG_PATH)
    result = ingest(ROOT, list(BAR_PATHS), catalog)
    records = result.records
    table = to_arrow_table(records)

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.parquet"
        write_parquet(table, path)
        read_back = read_parquet(path)
        round_tripped = table_to_records(read_back)
        duckdb_counts = query_duckdb(
            path,
            "SELECT product_root, count(*) FROM bars GROUP BY product_root ORDER BY product_root",
        )
        duckdb_total = query_duckdb(path, "SELECT count(*) FROM bars")[0][0]
        parquet_size = path.stat().st_size

    checks = {
        "row_count_preserved": len(round_tripped) == len(records),
        "values_byte_identical": round_tripped == [dict(r) for r in records],
        "integer_tick_prices_exact": all(
            isinstance(r[f], int) and r[f] == o[f]
            for r, o in zip(round_tripped, records, strict=True)
            for f in ("open_ticks", "high_ticks", "low_ticks", "close_ticks")
        ),
        "record_index_preserved": [r["record_index"] for r in round_tripped]
        == [r["record_index"] for r in records],
        "event_time_ns_preserved": [r["event_time_ns"] for r in round_tripped]
        == [r["event_time_ns"] for r in records],
        "schema_version_in_parquet_metadata": read_back.schema.metadata[b"aegis_schema_version"]
        == str(SCHEMA_VERSION).encode("utf-8"),
        "contract_identity_preserved": [r["contract_symbol"] for r in round_tripped]
        == [r["contract_symbol"] for r in records],
        "column_order_deterministic": table.column_names == list(NORMALIZED_COLUMNS),
        "duckdb_row_count_matches_arrow": duckdb_total == len(records),
    }
    if not all(checks.values()):
        failed = [name for name, ok in checks.items() if not ok]
        raise RuntimeError(f"columnar round-trip invariant(s) failed: {failed}; refusing to write evidence")

    payload = {
        "artifact": "columnar_roundtrip",
        "requirement": "AEGIS-230",
        **_provenance(),
        "input_paths": list(BAR_PATHS),
        "record_count": len(records),
        "parquet_bytes": parquet_size,
        "checks": checks,
        "duckdb_family_counts": duckdb_counts,
        "claim": (
            f"{len(records)} normalized futures_bar.v1 records (three synthetic product families) "
            "were written through the real production path -- to_arrow_table -> write_parquet -> "
            "read_parquet -> table_to_records, plus a DuckDB query over the identical Parquet file "
            "-- and every round-trip invariant AEGIS-230 requires was checked programmatically "
            "(row count, byte-identical values including exact integer tick prices, record_index, "
            "event_time_ns, contract identity, deterministic column order, schema_version carried "
            "in Parquet metadata, and DuckDB's row count agreeing with Arrow's). This generator "
            "raises rather than writing a claim it cannot support. All data is synthetic "
            "(DATA_AND_RESEARCH_POLICY); no claim is made about any real market."
        ),
    }

    out_dir = ROOT / "experiments/evidence/AEGIS-230"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "columnar_roundtrip.json"
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
