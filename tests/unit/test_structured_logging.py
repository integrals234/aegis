"""AEGIS-232 — machine-readable, correlated, deterministic, secret-free logs.

Every test here maps to a way logs stop being useful: unparseable, unjoinable,
unstable between runs, or carrying a credential to disk.
"""

from __future__ import annotations

import json

import jsonschema
import pytest
from common.clock import ManualClock, millis
from common.logging import (
    REDACTED,
    SCHEMA_VERSION,
    FileSink,
    Level,
    ListSink,
    StreamSink,
    StructuredLogger,
    load_schema,
    redact,
)

pytestmark = pytest.mark.unit


@pytest.fixture
def sink():
    return ListSink()


@pytest.fixture
def logger(sink):
    return StructuredLogger(
        name="exchange.sequencer",
        experiment_id="m0-logging",
        clock=ManualClock(1_700_000_000_000_000_000),
        sink=sink,
        level=Level.TRACE,
    )


@pytest.fixture
def schema(repo_root):
    return load_schema(repo_root)


def test_every_record_validates_against_the_schema(logger, sink, schema):
    logger.info("book opened", instrument="ESZ6", levels=10)
    logger.warn("sequence gap", expected=41, received=43)
    logger.error("feed disconnected")

    assert len(sink.records) == 3
    for record in sink.records:
        jsonschema.Draft202012Validator(schema).validate(record)


def test_each_line_is_one_parseable_json_object(logger, sink):
    logger.info("first")
    logger.info("second")
    for line in sink.lines:
        assert "\n" not in line
        assert isinstance(json.loads(line), dict)


def test_records_carry_the_experiment_id(logger, sink):
    """The same field the message envelope carries, so the two join directly."""
    logger.info("hello")
    assert sink.records[0]["experiment_id"] == "m0-logging"


def test_correlation_id_follows_a_causal_chain(logger, sink):
    order_logger = logger.bind(name="participant.oms", correlation_id="order-4711")
    order_logger.info("submitted")
    order_logger.info("acknowledged")

    assert all(record["correlation_id"] == "order-4711" for record in sink.records)
    assert all(record["logger"] == "participant.oms" for record in sink.records)


def test_bound_logger_keeps_its_own_sequence(sink):
    """Sequence orders one emitter's records; a shared counter would make the
    ordering depend on how components happened to interleave."""
    clock = ManualClock(0)
    parent = StructuredLogger("a", "exp", clock, sink)
    child = parent.bind(name="b")

    parent.info("one")
    child.info("two")
    parent.info("three")

    by_logger = {}
    for record in sink.records:
        by_logger.setdefault(record["logger"], []).append(record["sequence"])
    assert by_logger["a"] == [0, 1]
    assert by_logger["b"] == [0]


def test_sequence_breaks_timestamp_ties(sink):
    """Two records at the same nanosecond still have a defined order."""
    logger = StructuredLogger("t", "exp", ManualClock(42), sink)
    logger.info("first")
    logger.info("second")

    records = sink.records
    assert records[0]["timestamp_ns"] == records[1]["timestamp_ns"] == 42
    assert [r["sequence"] for r in records] == [0, 1]


def test_output_is_byte_identical_across_runs():
    """The clock is injected precisely so a fixture can be hashed (AEGIS-005)."""

    def run() -> list[str]:
        sink = ListSink()
        clock = ManualClock(1_700_000_000_000_000_000)
        logger = StructuredLogger("replay", "exp-determinism", clock, sink, level=Level.TRACE)
        for index in range(5):
            logger.debug("tick", index=index, phase="warmup" if index < 2 else "steady")
            clock.advance(millis(1))
        return sink.lines

    assert run() == run()


def test_field_order_does_not_depend_on_keyword_order(sink):
    """Otherwise two logically identical runs would hash differently."""
    logger = StructuredLogger("t", "exp", ManualClock(0), sink)
    logger.info("m", beta=2, alpha=1)
    logger.info("m", alpha=1, beta=2)
    assert sink.lines[0].replace('"sequence":0', '"sequence":1') == sink.lines[1]


@pytest.mark.parametrize(
    "key",
    [
        "password",
        "passwd",
        "api_key",
        "API-KEY",
        "aws_secret",
        "client_secret",
        "auth_token",
        "access_key",
        "db_credential",
        "private_key",
        "authorization",
    ],
)
def test_secret_shaped_fields_are_redacted(logger, sink, key):
    """A logger is the most common way a credential reaches disk."""
    logger.info("connecting", **{key: "EXAMPLEnotarealcredential"})
    assert sink.records[0]["fields"][key] == REDACTED
    assert "EXAMPLEnotarealcredential" not in sink.lines[0]


def test_ordinary_fields_survive_redaction(logger, sink):
    """A redactor that eats everything gets switched off."""
    logger.info("filled", instrument="ESZ6", quantity=5, aggressive=True, note=None)
    fields = sink.records[0]["fields"]
    assert fields == {"instrument": "ESZ6", "quantity": 5, "aggressive": True, "note": None}


def test_redact_is_usable_on_its_own():
    assert redact({"user": "ana", "token": "x"}) == {"user": "ana", "token": REDACTED}


def test_structured_values_are_flattened_to_scalars(logger, sink, schema):
    """The schema permits scalars only; a nested blob is unbounded on a hot path."""
    logger.info("state", book={"bid": 1, "ask": 2})
    jsonschema.Draft202012Validator(schema).validate(sink.records[0])
    assert isinstance(sink.records[0]["fields"]["book"], str)


def test_level_threshold_suppresses_quieter_records(sink):
    logger = StructuredLogger("t", "exp", ManualClock(0), sink, level=Level.WARN)
    assert logger.debug("ignored") is None
    assert logger.info("ignored") is None
    assert logger.warn("kept") is not None
    assert logger.error("kept") is not None
    assert [r["level"] for r in sink.records] == ["warn", "error"]


def test_suppressed_records_do_not_consume_a_sequence_number(sink):
    """Otherwise the sequence would have gaps whose meaning depends on the level."""
    logger = StructuredLogger("t", "exp", ManualClock(0), sink, level=Level.WARN)
    logger.debug("dropped")
    logger.warn("kept")
    assert sink.records[0]["sequence"] == 0


def test_level_parsing_rejects_an_unknown_name():
    with pytest.raises(ValueError, match="unknown log level"):
        Level.parse("verbose")


def test_level_parsing_is_case_insensitive():
    assert Level.parse("WARN") is Level.WARN
    assert Level.parse(" info ") is Level.INFO


def test_logger_requires_an_experiment_id(sink):
    with pytest.raises(ValueError, match="experiment_id is required"):
        StructuredLogger("t", "", ManualClock(0), sink)


def test_logger_requires_a_name(sink):
    with pytest.raises(ValueError, match="logger name is required"):
        StructuredLogger("", "exp", ManualClock(0), sink)


def test_schema_version_is_present_on_every_record(logger, sink):
    """A reader must be able to reject a shape it does not understand."""
    logger.info("m")
    assert sink.records[0]["schema_version"] == SCHEMA_VERSION


def test_file_sink_appends_one_line_per_record(tmp_path):
    path = tmp_path / "logs/run.jsonl"
    logger = StructuredLogger("t", "exp", ManualClock(7), FileSink(path))
    logger.info("one")
    logger.info("two")

    lines = path.read_text(encoding="utf-8").splitlines()
    assert len(lines) == 2
    assert json.loads(lines[1])["message"] == "two"


def test_stream_sink_writes_to_an_open_stream(tmp_path):
    path = tmp_path / "stream.jsonl"
    with path.open("w", encoding="utf-8") as handle:
        StructuredLogger("t", "exp", ManualClock(1), StreamSink(handle)).info("streamed")
    assert json.loads(path.read_text(encoding="utf-8"))["message"] == "streamed"


def test_timestamp_comes_from_the_injected_clock(sink):
    clock = ManualClock(5)
    logger = StructuredLogger("t", "exp", clock, sink)
    logger.info("first")
    clock.advance(millis(3))
    logger.info("second")

    assert [r["timestamp_ns"] for r in sink.records] == [5, 3_000_005]
