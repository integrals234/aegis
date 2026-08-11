"""The normalized multi-market futures schema, `futures_bar.v1` (AEGIS-026).

One versioned schema every product family's data is normalized into, wired
through the existing :class:`~data.schema_registry.SchemaRegistry`
(AEGIS-230's M0 half) rather than a second, parallel validator -- the same
discipline `python/futures/instruments.py` follows for the product catalog,
now via the registry that already exists for exactly this purpose.

No floating-point prices: OHLC and settlement are integer tick counts.
`python/futures/ingest.py` is the only writer of this schema; `columnar.py`
(M2 slice 5) and `quality.py` read it without re-deriving its shape.
"""

from __future__ import annotations

from pathlib import Path
from typing import Final

from data.schema_registry import Compatibility, SchemaRegistry

__all__ = [
    "NORMALIZED_COLUMNS",
    "SCHEMA_NAME",
    "SCHEMA_PATH",
    "SCHEMA_VERSION",
    "build_registry",
]

SCHEMA_NAME: Final[str] = "futures_bar"
SCHEMA_VERSION: Final[int] = 1
SCHEMA_PATH: Final[str] = "configs/schemas/futures_bar.v1.json"

# Explicit, fixed column order. Shared by every writer of this shape -- CSV
# fixtures, columnar output (AEGIS-230) -- so none of them depends on dict
# insertion order or hash order.
NORMALIZED_COLUMNS: Final[tuple[str, ...]] = (
    "schema_version",
    "venue",
    "product_root",
    "contract_symbol",
    "event_time_ns",
    "open_ticks",
    "high_ticks",
    "low_ticks",
    "close_ticks",
    "volume",
    "open_interest",
    "settlement_price_ticks",
    "source_sequence",
    "record_index",
)


def build_registry(root: Path) -> SchemaRegistry:
    """A :class:`SchemaRegistry` with exactly `futures_bar.v1` registered."""
    registry = SchemaRegistry()
    registry.register_file(root / SCHEMA_PATH, compatibility=Compatibility.BACKWARD)
    return registry
