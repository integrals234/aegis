"""Versioned message envelope and canonical encoding (ADR-0002), Python peer.

Byte-for-byte the same format as ``cpp/events/envelope.hpp``. A shared golden
fixture (``tests/unit/fixtures/envelope/golden.json``) holds both
implementations to it, so a change to one that is not made to the other fails a
test rather than surfacing as a corrupt recording months later.

Canonical means one value has exactly one byte representation: fixed field
order, no optional fields, fixed-width little-endian integers, length-prefixed
strings, no floating point, no map iteration order, no locale. That is the
property AEGIS-005 needs — without it, "the two runs differ" is a question about
the encoder rather than about the engine.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import Enum, IntEnum

from common.clock import EventTime, serialize_nanos

ENVELOPE_SCHEMA_VERSION = 1

MAX_STRING_LENGTH = 0xFFFF
MAX_PAYLOAD_LENGTH = 0xFFFFFFFF

_U16 = struct.Struct("<H")
_U32 = struct.Struct("<I")
_U64 = struct.Struct("<Q")

FIXED_PREFIX_LENGTH = 2 + 2 + 8 + 8 + 8


class MessageType(IntEnum):
    """Message types. Numbers are permanent.

    A retired type keeps its number forever: a recorded stream outlives the code
    that wrote it, so reusing a number would make old recordings decode into the
    wrong type. M0 defines one — the platform envelope. Domain types arrive with
    the exchange in M1 (1..999) and the participant in M3 (1000..1999).
    """

    UNSPECIFIED = 0


class DecodeError(Enum):
    """Why a decode failed.

    Returned as an exception payload rather than a silent None: a malformed
    message is an expected event a feed handler must count and continue past,
    and it must be able to say which kind it was.
    """

    TRUNCATED = "message ends before its declared fields do"
    UNSUPPORTED_SCHEMA_VERSION = "envelope schema version is not understood by this build"
    UNKNOWN_MESSAGE_TYPE = "message type is not defined in this build"
    LENGTH_OVERFLOW = "declared payload length exceeds the bytes supplied"
    TRAILING_BYTES = "message carries bytes beyond its declared payload"


class EnvelopeDecodeError(ValueError):
    def __init__(self, reason: DecodeError) -> None:
        super().__init__(f"{reason.name.lower()}: {reason.value}")
        self.reason = reason


class EnvelopeEncodeError(ValueError):
    """A value cannot be represented in the canonical form."""


@dataclass(frozen=True, slots=True)
class Envelope:
    """The header every AEGIS message carries."""

    sequence: int = 0
    stream_id: int = 0
    event_time: EventTime = field(default_factory=lambda: EventTime(0))
    producer_id: str = ""
    experiment_id: str = ""
    correlation_id: str = ""
    payload: bytes = b""
    message_type: MessageType = MessageType.UNSPECIFIED
    schema_version: int = ENVELOPE_SCHEMA_VERSION


def _put_string(parts: list[bytes], text: str) -> None:
    encoded = text.encode("utf-8")
    if len(encoded) > MAX_STRING_LENGTH:
        raise EnvelopeEncodeError(
            f"string field is {len(encoded)} bytes, over the {MAX_STRING_LENGTH}-byte limit; "
            "the length prefix is 16 bits and silently truncating would corrupt the frame"
        )
    parts.append(_U16.pack(len(encoded)))
    parts.append(encoded)


def encode(envelope: Envelope) -> bytes:
    """Encode into the canonical byte form."""
    if len(envelope.payload) > MAX_PAYLOAD_LENGTH:
        raise EnvelopeEncodeError(f"payload exceeds {MAX_PAYLOAD_LENGTH} bytes")

    parts: list[bytes] = [
        _U16.pack(envelope.schema_version),
        _U16.pack(int(envelope.message_type)),
        _U64.pack(envelope.sequence),
        _U64.pack(envelope.stream_id),
        # serialize_nanos refuses a MonotonicTime: its origin differs per process,
        # so a recorded one would replay to a different value.
        _U64.pack(serialize_nanos(envelope.event_time) & 0xFFFFFFFFFFFFFFFF),
    ]
    _put_string(parts, envelope.producer_id)
    _put_string(parts, envelope.experiment_id)
    _put_string(parts, envelope.correlation_id)
    parts.append(_U32.pack(len(envelope.payload)))
    parts.append(envelope.payload)
    return b"".join(parts)


def _take_string(data: bytes, offset: int) -> tuple[str, int]:
    if offset + 2 > len(data):
        raise EnvelopeDecodeError(DecodeError.TRUNCATED)
    (length,) = _U16.unpack_from(data, offset)
    offset += 2
    if offset + length > len(data):
        raise EnvelopeDecodeError(DecodeError.TRUNCATED)
    return data[offset : offset + length].decode("utf-8"), offset + length


def decode(data: bytes) -> Envelope:
    """Decode a canonical byte form, raising EnvelopeDecodeError on any problem."""
    if len(data) < FIXED_PREFIX_LENGTH:
        raise EnvelopeDecodeError(DecodeError.TRUNCATED)

    offset = 0
    (schema_version,) = _U16.unpack_from(data, offset)
    offset += 2
    if schema_version != ENVELOPE_SCHEMA_VERSION:
        # Refused, not tolerated: guessing at an unknown layout yields plausible
        # messages that are wrong, and the run that follows looks normal.
        raise EnvelopeDecodeError(DecodeError.UNSUPPORTED_SCHEMA_VERSION)

    (raw_type,) = _U16.unpack_from(data, offset)
    offset += 2
    try:
        message_type = MessageType(raw_type)
    except ValueError:
        raise EnvelopeDecodeError(DecodeError.UNKNOWN_MESSAGE_TYPE) from None

    (sequence,) = _U64.unpack_from(data, offset)
    offset += 8
    (stream_id,) = _U64.unpack_from(data, offset)
    offset += 8
    (raw_time,) = _U64.unpack_from(data, offset)
    offset += 8
    event_nanos = raw_time - 0x1_0000_0000_0000_0000 if raw_time >= 0x8000_0000_0000_0000 else raw_time

    producer_id, offset = _take_string(data, offset)
    experiment_id, offset = _take_string(data, offset)
    correlation_id, offset = _take_string(data, offset)

    if offset + 4 > len(data):
        raise EnvelopeDecodeError(DecodeError.TRUNCATED)
    (payload_length,) = _U32.unpack_from(data, offset)
    offset += 4
    if offset + payload_length > len(data):
        raise EnvelopeDecodeError(DecodeError.LENGTH_OVERFLOW)
    payload = data[offset : offset + payload_length]
    offset += payload_length

    if offset != len(data):
        # Trailing bytes mean the framing is wrong upstream; accepting them
        # silently would let a desynchronised stream keep decoding.
        raise EnvelopeDecodeError(DecodeError.TRAILING_BYTES)

    return Envelope(
        schema_version=schema_version,
        message_type=message_type,
        sequence=sequence,
        stream_id=stream_id,
        event_time=EventTime(event_nanos),
        producer_id=producer_id,
        experiment_id=experiment_id,
        correlation_id=correlation_id,
        payload=payload,
    )
