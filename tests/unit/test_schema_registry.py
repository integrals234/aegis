"""AEGIS-230 — versioned schemas, and round trips through the M0 codecs.

The columnar half of AEGIS-230 (Parquet/Arrow/DuckDB) is deferred to M2, the
first milestone with columnar data to interchange; adding it now would mean
inventing the futures schema that AEGIS-026 owns. What is tested here is the
part every format depends on: a record says which version it was written under,
and a reader refuses a version it does not know.
"""

from __future__ import annotations

import pytest
from data.schema_registry import (
    Compatibility,
    SchemaError,
    SchemaRegistry,
    UnknownSchemaVersion,
    from_csv,
    from_jsonl,
    to_csv,
    to_jsonl,
)

pytestmark = pytest.mark.unit

V1 = {
    "type": "object",
    "additionalProperties": False,
    "required": ["schema_version", "symbol", "size"],
    "properties": {
        "schema_version": {"type": "integer", "enum": [1]},
        "symbol": {"type": "string", "minLength": 1},
        "size": {"type": "integer", "minimum": 0},
    },
}

V2 = {
    "type": "object",
    "additionalProperties": False,
    "required": ["schema_version", "symbol", "size", "venue"],
    "properties": {
        "schema_version": {"type": "integer", "enum": [2]},
        "symbol": {"type": "string", "minLength": 1},
        "size": {"type": "integer", "minimum": 0},
        "venue": {"type": "string"},
    },
}


@pytest.fixture
def registry():
    registry = SchemaRegistry()
    registry.register("quote", 1, V1, Compatibility.BREAKING)
    registry.register("quote", 2, V2, Compatibility.BACKWARD)
    return registry


def test_record_is_validated_against_the_version_it_declares(registry):
    registry.validate("quote", {"schema_version": 1, "symbol": "ESZ6", "size": 5})
    registry.validate("quote", {"schema_version": 2, "symbol": "ESZ6", "size": 5, "venue": "CME"})


def test_versions_coexist_so_a_migration_can_be_gradual(registry):
    assert registry.versions("quote") == [1, 2]
    assert registry.latest("quote").version == 2


def test_a_record_without_a_version_is_refused(registry):
    with pytest.raises(SchemaError, match="refuses to guess"):
        registry.validate("quote", {"symbol": "ESZ6", "size": 5})


def test_an_unknown_version_is_refused_not_reinterpreted(registry):
    """Reading newer data under older field meanings yields research results
    that are wrong in a way no test detects."""
    with pytest.raises(UnknownSchemaVersion) as excinfo:
        registry.validate("quote", {"schema_version": 3, "symbol": "ESZ6", "size": 5})
    assert excinfo.value.found == 3
    assert excinfo.value.known == (1, 2)
    assert "looks valid and is wrong" in str(excinfo.value)


def test_a_v2_record_is_not_accepted_under_v1_rules(registry):
    """The version selects the schema, so a v1 record with a v2 field fails."""
    with pytest.raises(SchemaError, match="venue"):
        registry.validate("quote", {"schema_version": 1, "symbol": "ESZ6", "size": 5, "venue": "CME"})


def test_invalid_record_names_every_offending_field(registry):
    with pytest.raises(SchemaError) as excinfo:
        registry.validate("quote", {"schema_version": 1, "symbol": "", "size": -1})
    message = str(excinfo.value)
    assert "symbol" in message
    assert "size" in message


def test_registering_a_version_twice_is_refused(registry):
    """Otherwise one declared version would mean two things by import order."""
    with pytest.raises(SchemaError, match="already registered"):
        registry.register("quote", 1, V1)


def test_a_schema_without_a_version_property_is_refused():
    registry = SchemaRegistry()
    with pytest.raises(SchemaError, match="must declare a 'schema_version' property"):
        registry.register("bad", 1, {"type": "object", "properties": {"x": {"type": "integer"}}})


def test_compatibility_is_recorded_per_version(registry):
    assert registry.get("quote", 2).compatibility is Compatibility.BACKWARD
    assert registry.get("quote", 1).compatibility is Compatibility.BREAKING


def test_unknown_schema_name_is_reported(registry):
    with pytest.raises(SchemaError, match="no schema registered"):
        registry.latest("does-not-exist")


def test_schema_file_must_name_itself(tmp_path):
    path = tmp_path / "anonymous.json"
    path.write_text('{"type": "object"}', encoding="utf-8")
    with pytest.raises(SchemaError, match="cannot name itself"):
        SchemaRegistry().register_file(path)


def test_committed_manifest_schema_registers_from_its_file(repo_root):
    registry = SchemaRegistry()
    registered = registry.register_file(repo_root / "configs/schemas/experiment_manifest.v1.json")
    assert registered.name == "experiment_manifest"
    assert registered.version == 1


# ---------------------------------------------------------------------------
# Codecs
# ---------------------------------------------------------------------------

RECORDS = [
    {"schema_version": 1, "symbol": "ESZ6", "size": 5},
    {"schema_version": 1, "symbol": "CLF7", "size": 0},
]


def test_jsonl_round_trip(registry):
    decoded = from_jsonl(to_jsonl(RECORDS))
    assert decoded == RECORDS
    for record in decoded:
        registry.validate("quote", record)


def test_jsonl_is_stable_regardless_of_key_order():
    """A dataset whose bytes depend on insertion order cannot be diffed or hashed."""
    a = to_jsonl([{"schema_version": 1, "symbol": "ESZ6", "size": 5}])
    b = to_jsonl([{"size": 5, "symbol": "ESZ6", "schema_version": 1}])
    assert a == b


def test_jsonl_reports_the_line_that_failed():
    with pytest.raises(SchemaError, match="line 2"):
        from_jsonl('{"a": 1}\nnot json\n')


def test_jsonl_rejects_a_non_object_line():
    with pytest.raises(SchemaError, match="must be a JSON object"):
        from_jsonl("[1, 2, 3]\n")


def test_csv_round_trip(registry):
    columns = ("schema_version", "symbol", "size")
    decoded = from_csv(to_csv(RECORDS, columns), integer_columns=("schema_version", "size"))
    assert decoded == RECORDS
    for record in decoded:
        registry.validate("quote", record)


def test_csv_column_order_is_explicit():
    """A CSV whose columns come from the first record changes shape with the data."""
    text = to_csv(RECORDS, ("size", "symbol", "schema_version"))
    assert text.splitlines()[0] == "size,symbol,schema_version"


def test_csv_rejects_a_record_missing_a_column():
    with pytest.raises(SchemaError, match="missing columns"):
        to_csv([{"symbol": "ESZ6"}], ("symbol", "size"))


def test_csv_integer_columns_are_stated_not_guessed():
    """Guessing would turn an instrument code like '007' into 7."""
    text = "code,size\n007,5\n"
    assert from_csv(text, integer_columns=("size",)) == [{"code": "007", "size": 5}]


def test_csv_reports_a_non_integer_in_an_integer_column():
    with pytest.raises(SchemaError, match="not an integer"):
        from_csv("size\nmany\n", integer_columns=("size",))


def test_committed_sample_round_trips_through_the_csv_codec(repo_root):
    """The committed sample is real input to the codec, not a decorative file."""
    text = (repo_root / "data_samples/synthetic_book_events.csv").read_text(encoding="utf-8")
    records = from_csv(text, integer_columns=("schema_version", "sequence", "event_time_ns",
                                              "price_ticks", "size"))
    assert len(records) == 200
    assert all(record["schema_version"] == 1 for record in records)
    columns = ("schema_version", "sequence", "event_time_ns", "side", "price_ticks", "size")
    assert to_csv(records, columns) == text
