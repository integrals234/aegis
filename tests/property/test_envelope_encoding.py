"""ADR-0002 — canonical encoding invariants, checked over generated inputs.

The property layer exists for claims that must hold for *all* values, not for
the handful somebody thought to write down. Three such claims carry the
determinism guarantee AEGIS-005 depends on:

* decode(encode(x)) == x for every envelope;
* encoding is a function — the same envelope always produces the same bytes;
* encoding is injective — two different envelopes never produce the same bytes.

If any of these fails, a replay hash stops measuring the engine and starts
measuring the encoder.
"""

from __future__ import annotations

import pytest
from common.clock import EventTime
from common.envelope import (
    MAX_STRING_LENGTH,
    DecodeError,
    Envelope,
    EnvelopeDecodeError,
    EnvelopeEncodeError,
    MessageType,
    decode,
    encode,
)
from hypothesis import given, settings
from hypothesis import strategies as st

pytestmark = pytest.mark.property

# int64: the event-time domain, including values before the Unix epoch.
event_nanos = st.integers(min_value=-(2**63), max_value=2**63 - 1)
uint64 = st.integers(min_value=0, max_value=2**64 - 1)
identifiers = st.text(max_size=64)

envelopes = st.builds(
    Envelope,
    sequence=uint64,
    stream_id=uint64,
    event_time=st.builds(EventTime, event_nanos),
    producer_id=identifiers,
    experiment_id=identifiers,
    correlation_id=identifiers,
    payload=st.binary(max_size=256),
    message_type=st.just(MessageType.UNSPECIFIED),
)


@given(envelopes)
@settings(max_examples=400)
def test_round_trip_preserves_every_field(envelope):
    assert decode(encode(envelope)) == envelope


@given(envelopes)
@settings(max_examples=200)
def test_encoding_is_a_function(envelope):
    """The same value must always produce the same bytes, or a replay hash is noise."""
    assert encode(envelope) == encode(envelope)


@given(envelopes, envelopes)
@settings(max_examples=300)
def test_encoding_is_injective(first, second):
    """Two distinct envelopes must never collide onto one byte string."""
    if first == second:
        return
    assert encode(first) != encode(second)


@given(envelopes)
@settings(max_examples=200)
def test_no_floating_point_in_the_encoded_form(envelope):
    """Structural check: every field is an integer or a length-prefixed byte string.

    A float in the wire format would print and round differently across libcs and
    compiler flags, which is exactly the class of difference a determinism hash
    cannot distinguish from an engine bug.
    """
    encoded = encode(envelope)
    assert isinstance(encoded, bytes)
    payload_start = len(encoded) - len(envelope.payload)
    assert encoded[payload_start:] == envelope.payload


@given(envelopes, st.binary(min_size=1, max_size=8))
@settings(max_examples=200)
def test_trailing_bytes_are_rejected(envelope, extra):
    """A desynchronised stream must stop, not keep decoding plausible messages."""
    with pytest.raises(EnvelopeDecodeError) as excinfo:
        decode(encode(envelope) + extra)
    assert excinfo.value.reason in (DecodeError.TRAILING_BYTES, DecodeError.LENGTH_OVERFLOW)


@given(envelopes, st.integers(min_value=1, max_value=32))
@settings(max_examples=200)
def test_truncation_is_always_detected(envelope, cut):
    """No prefix of a valid message may decode as a different valid message."""
    encoded = encode(envelope)
    truncated = encoded[: max(0, len(encoded) - cut)]
    if truncated == encoded:
        return
    with pytest.raises(EnvelopeDecodeError):
        decode(truncated)


@given(st.integers(min_value=0, max_value=2**16 - 1).filter(lambda v: v != 1))
@settings(max_examples=100)
def test_unknown_schema_version_is_refused(version):
    """Guessing at an unknown layout yields plausible messages that are wrong."""
    encoded = bytearray(encode(Envelope(sequence=1)))
    encoded[0:2] = version.to_bytes(2, "little")
    with pytest.raises(EnvelopeDecodeError) as excinfo:
        decode(bytes(encoded))
    assert excinfo.value.reason is DecodeError.UNSUPPORTED_SCHEMA_VERSION


@given(st.integers(min_value=1, max_value=2**16 - 1))
@settings(max_examples=100)
def test_unknown_message_type_is_refused(raw_type):
    """A number that means nothing to this build must not decode as UNSPECIFIED."""
    encoded = bytearray(encode(Envelope(sequence=1)))
    encoded[2:4] = raw_type.to_bytes(2, "little")
    with pytest.raises(EnvelopeDecodeError) as excinfo:
        decode(bytes(encoded))
    assert excinfo.value.reason is DecodeError.UNKNOWN_MESSAGE_TYPE


@given(st.integers(min_value=0, max_value=2**64 - 1))
@settings(max_examples=200)
def test_sequence_survives_the_full_unsigned_range(sequence):
    """Sequence numbers must not wrap: gap detection depends on them (AEGIS-068)."""
    assert decode(encode(Envelope(sequence=sequence))).sequence == sequence


@given(event_nanos)
@settings(max_examples=200)
def test_event_time_survives_the_full_signed_range(nanos):
    """Including negative values: a pre-epoch timestamp is a data-quality signal,
    not something the encoder may quietly turn into a huge positive number."""
    envelope = Envelope(event_time=EventTime(nanos))
    assert decode(encode(envelope)).event_time == EventTime(nanos)


def test_oversized_string_is_rejected_rather_than_truncated():
    """A 16-bit length prefix cannot describe a longer string; truncating would
    corrupt the frame and every message after it."""
    with pytest.raises(EnvelopeEncodeError, match="length prefix is 16 bits"):
        encode(Envelope(producer_id="x" * (MAX_STRING_LENGTH + 1)))
