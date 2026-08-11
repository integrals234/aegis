#!/usr/bin/env python3
"""Generate the canonical replay stream fixture from the committed bar samples.

The fixture is what `aegis_replay_run` (cpp/replay, M2 slice 14) replays and
what `python/common/determinism.py`'s ``futures_replay`` producer points at.
It is *derived*, never hand-written: every record comes from the real
`futures.ingest.ingest` pipeline over the committed `data_samples/futures/bars/`
files, carrying the `record_index` that ingestion assigned, sorted into the
canonical replay order by the real `futures.replay.sort_canonical`.

Committing a derived file risks it drifting from the code that derives it, so
`tests/replay/test_futures_replay_determinism.py` regenerates it in memory and
fails if the committed bytes differ. The fixture is a convenience for a
dependency-free consumer (``python-common`` may not import ``python-futures``),
not a second source of truth.

Regenerate with: python3 tools/make_replay_fixture.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from futures.ingest import ingest
from futures.instruments import DEFAULT_CATALOG_PATH, load_catalog
from futures.replay import sort_canonical

# The same three committed families the end-to-end integration test loads.
BAR_PATHS = (
    "data_samples/futures/bars/eqx.csv",
    "data_samples/futures/bars/clx.jsonl",
    "data_samples/futures/bars/srx.csv",
)

FIXTURE_RELATIVE_PATH = "tests/unit/fixtures/replay/futures_canonical_stream.jsonl"

# Exactly the four fields cpp/replay/replay_event.hpp's canonical order names.
# The bar records carry more (prices, volume, open interest); replay orders and
# identifies records, it does not price them, so nothing else belongs here.
CANONICAL_FIELDS = ("event_time_ns", "source_sequence", "contract_symbol", "record_index")


def build_stream(root: Path = ROOT) -> str:
    """The canonical stream's exact bytes, from production code only."""
    catalog = load_catalog(root, DEFAULT_CATALOG_PATH)
    result = ingest(root, list(BAR_PATHS), catalog)
    if result.rejections:
        raise RuntimeError(f"committed bar samples must ingest cleanly; got {result.rejections}")
    ordered = sort_canonical(result.records)
    return "".join(
        json.dumps({field: record[field] for field in CANONICAL_FIELDS}, sort_keys=True) + "\n"
        for record in ordered
    )


def main(argv: list[str] | None = None) -> int:
    del argv
    stream = build_stream()
    out_path = ROOT / FIXTURE_RELATIVE_PATH
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(stream, encoding="utf-8")
    print(f"wrote {FIXTURE_RELATIVE_PATH} ({len(stream.splitlines())} records, {len(stream)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
