"""ADR-0002 — the Python encoder must produce the golden bytes.

``tests/cpp/unit/test_envelope.cpp`` asserts the same fixture. Holding both
implementations to one committed byte string is what keeps them from drifting:
without it, a change to one encoder shows up as a corrupt recording months
later, at which point the recordings that matter were made with the old format.

Regenerating the fixture to make a test pass is a change to the wire format and
requires an ADR, not a commit message.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from common.clock import EventTime
from common.envelope import ENVELOPE_SCHEMA_VERSION, Envelope, MessageType, decode, encode

pytestmark = pytest.mark.unit

GOLDEN = json.loads(
    (Path(__file__).parent / "fixtures/envelope/golden.json").read_text(encoding="utf-8")
)
CASES = GOLDEN["cases"]


def build(fields: dict) -> Envelope:
    return Envelope(
        sequence=fields["sequence"],
        stream_id=fields["stream_id"],
        event_time=EventTime(fields["event_time_ns"]),
        producer_id=fields["producer_id"],
        experiment_id=fields["experiment_id"],
        correlation_id=fields["correlation_id"],
        payload=bytes.fromhex(fields["payload_hex"]),
        message_type=MessageType.UNSPECIFIED,
    )


@pytest.mark.parametrize("case", CASES, ids=[c["name"] for c in CASES])
def test_encoder_matches_the_golden_bytes(case):
    assert encode(build(case["fields"])).hex() == case["encoded_hex"], case["why"]


@pytest.mark.parametrize("case", CASES, ids=[c["name"] for c in CASES])
def test_golden_bytes_decode_back_to_the_case(case):
    assert decode(bytes.fromhex(case["encoded_hex"])) == build(case["fields"])


def test_fixture_pins_the_schema_version_and_message_type():
    """If either changes, every recorded stream's framing changes with it."""
    assert GOLDEN["schema_version"] == ENVELOPE_SCHEMA_VERSION
    assert GOLDEN["message_type"] == int(MessageType.UNSPECIFIED)


def test_fixture_covers_the_boundaries_that_matter():
    names = {case["name"] for case in CASES}
    assert {"minimal", "negative_event_time", "max_sequence", "unicode_identifiers"} <= names


def test_every_case_records_why_it_exists():
    """A fixture case nobody can justify is a case nobody will maintain."""
    for case in CASES:
        assert case["why"].strip()
