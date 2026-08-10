"""M2 slice 4 -- AEGIS-026: the `futures_bar.v1` normalized schema.

Wired through the existing `python/data/schema_registry.SchemaRegistry`
(AEGIS-230's M0 half), not a second, parallel validator -- this file proves
that wiring rather than re-testing `SchemaRegistry` itself
(`tests/unit/test_schema_registry.py` already does that).
"""

from __future__ import annotations

from pathlib import Path

import pytest
from data.schema_registry import SchemaError, UnknownSchemaVersion
from futures.schema import NORMALIZED_COLUMNS, SCHEMA_NAME, SCHEMA_VERSION, build_registry

pytestmark = pytest.mark.unit


@pytest.fixture
def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def valid_record() -> dict[str, object]:
    return {
        "schema_version": 1,
        "venue": "SYNX",
        "product_root": "EQX",
        "contract_symbol": "SYNX:EQX:2026H",
        "event_time_ns": 1_773_144_000_000_000_000,
        "open_ticks": 20000,
        "high_ticks": 20004,
        "low_ticks": 19998,
        "close_ticks": 20001,
        "volume": 1000,
        "open_interest": 5000,
        "settlement_price_ticks": 20001,
        "source_sequence": 0,
        "record_index": 0,
    }


def test_registry_registers_exactly_the_futures_bar_schema(repo_root: Path) -> None:
    registry = build_registry(repo_root)
    assert registry.versions(SCHEMA_NAME) == [SCHEMA_VERSION]


def test_valid_record_passes(repo_root: Path) -> None:
    registry = build_registry(repo_root)
    registry.validate(SCHEMA_NAME, valid_record())


def test_nullable_fields_accept_null(repo_root: Path) -> None:
    registry = build_registry(repo_root)
    record = valid_record()
    record["volume"] = None
    record["open_interest"] = None
    record["settlement_price_ticks"] = None
    registry.validate(SCHEMA_NAME, record)


def test_missing_required_field_rejected(repo_root: Path) -> None:
    registry = build_registry(repo_root)
    record = valid_record()
    del record["event_time_ns"]
    with pytest.raises(SchemaError):
        registry.validate(SCHEMA_NAME, record)


def test_additional_property_rejected(repo_root: Path) -> None:
    registry = build_registry(repo_root)
    record = valid_record()
    record["unexpected_field"] = "x"
    with pytest.raises(SchemaError):
        registry.validate(SCHEMA_NAME, record)


def test_unknown_schema_version_rejected(repo_root: Path) -> None:
    registry = build_registry(repo_root)
    record = valid_record()
    record["schema_version"] = 2
    with pytest.raises(UnknownSchemaVersion):
        registry.validate(SCHEMA_NAME, record)


def test_non_integer_price_rejected(repo_root: Path) -> None:
    registry = build_registry(repo_root)
    record = valid_record()
    record["open_ticks"] = "20000"  # a string, not the required integer
    with pytest.raises(SchemaError):
        registry.validate(SCHEMA_NAME, record)


def test_negative_volume_rejected(repo_root: Path) -> None:
    registry = build_registry(repo_root)
    record = valid_record()
    record["volume"] = -1
    with pytest.raises(SchemaError):
        registry.validate(SCHEMA_NAME, record)


def test_normalized_columns_cover_every_required_field(repo_root: Path) -> None:
    import json

    schema = json.loads((repo_root / "configs/schemas/futures_bar.v1.json").read_text(encoding="utf-8"))
    assert set(NORMALIZED_COLUMNS) == set(schema["required"])
