#!/usr/bin/env python3
"""Regenerate the committed synthetic sample (AEGIS-236).

The sample is generated rather than sliced from a real feed. A small excerpt of
licensed market data is still licensed, and AEGIS-236 permits committing only
redistributable samples — so the redistributable thing is one that contains no
market data at all.

The generator is seeded, so re-running it reproduces the committed file byte for
byte and a diff means somebody changed the generator.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from data.schema_registry import to_csv

COLUMNS = ("schema_version", "sequence", "event_time_ns", "side", "price_ticks", "size")
BASE_EVENT_NANOS = 1_700_000_000_000_000_000


def generate(rows: int, seed: int) -> str:
    rng = random.Random(seed)
    records = []
    for sequence in range(rows):
        records.append(
            {
                "schema_version": 1,
                "sequence": sequence,
                "event_time_ns": BASE_EVENT_NANOS + sequence * 1_000_000,
                "side": rng.choice(("bid", "ask")),
                # Integer ticks, never a float price: a float in an interchange
                # file prints differently across writers and stops being diffable.
                "price_ticks": 500_000 + rng.randrange(-50, 51),
                "size": rng.randrange(1, 25),
            }
        )
    return to_csv(records, COLUMNS)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=int, default=200)
    parser.add_argument("--seed", type=int, default=20260806)
    parser.add_argument("--output", type=Path, default=ROOT / "data_samples/synthetic_book_events.csv")
    args = parser.parse_args(argv)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(args.rows, args.seed), encoding="utf-8")
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
