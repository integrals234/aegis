"""Exchange domain message vocabulary (ADR-0009), Python peer.

Byte-for-byte the same format as ``cpp/events/exchange_messages.hpp``: fixed
field order, fixed-width little-endian, no floating point (envelope.py's
canonical rules, which this mirrors the same way ``python/common/envelope.py``
mirrors ``cpp/events/envelope.hpp``).

Both ``encode`` and ``decode`` are implemented for every command and event —
not decode-only — so the hypothesis round-trip pattern of
``tests/property/test_envelope_encoding.py`` is legitimate at the
exchange-message level too (a C++-authored golden hex fixture is a separate,
second check that the two languages agree on one canonical byte string, which
a round trip alone cannot prove).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

_U8 = struct.Struct("<B")
_U32 = struct.Struct("<I")
_U64 = struct.Struct("<Q")
_I64 = struct.Struct("<q")


class ExchangeMessageDecodeError(ValueError):
    """A payload could not be decoded: truncated, trailing bytes, or an
    unknown enumerator where a closed set was expected."""


class Side(IntEnum):
    BUY = 0
    SELL = 1


class OrderType(IntEnum):
    LIMIT = 0
    MARKET = 1


class RejectReason(IntEnum):
    """Closed set of validation-failure reasons (AEGIS-035, ADR-0011)."""

    UNKNOWN_INSTRUMENT = 0
    DUPLICATE_CLIENT_ORDER_ID = 1
    NON_POSITIVE_QUANTITY = 2
    QUANTITY_NOT_ON_LOT = 3
    QUANTITY_OUT_OF_RANGE = 4
    PRICE_NOT_ON_TICK = 5
    PRICE_OUT_OF_BAND = 6
    PRICE_ON_MARKET_ORDER = 7
    UNKNOWN_ORDER_ID = 8
    NOT_ORDER_OWNER = 9
    MODIFY_BELOW_FILLED = 10
    MALFORMED_MESSAGE = 11


class TerminationReason(IntEnum):
    """Every order's removal has exactly one event and exactly one reason."""

    FILLED = 0
    CANCELED = 1
    RESIDUAL_CANCELED = 2
    REPLACED = 3


# --------------------------------------------------------------------- commands


@dataclass(frozen=True, slots=True)
class NewOrderCommand:
    instrument_id: int = 0
    participant_id: int = 0
    client_order_id: int = 0
    side: Side = Side.BUY
    order_type: OrderType = OrderType.LIMIT
    price_units: int = 0  # ignored (must be 0) when order_type is MARKET
    quantity_units: int = 0


@dataclass(frozen=True, slots=True)
class CancelOrderCommand:
    instrument_id: int = 0
    participant_id: int = 0
    order_id: int = 0


@dataclass(frozen=True, slots=True)
class ModifyOrderCommand:
    instrument_id: int = 0
    participant_id: int = 0
    order_id: int = 0
    new_price_units: int = 0
    new_quantity_units: int = 0


# ----------------------------------------------------------------------- events


@dataclass(frozen=True, slots=True)
class OrderAcceptedEvent:
    causing_command_sequence: int = 0
    order_id: int = 0
    instrument_id: int = 0
    participant_id: int = 0
    client_order_id: int = 0
    side: Side = Side.BUY
    order_type: OrderType = OrderType.LIMIT
    price_units: int = 0
    quantity_units: int = 0  # original_quantity at acceptance


@dataclass(frozen=True, slots=True)
class OrderRejectedEvent:
    """A rejection from a NewOrder (client_order_id identifies the rejected
    submission, order_id is 0) or from a CancelOrder/ModifyOrder (order_id
    identifies the targeted live order, client_order_id is 0). Exactly one of
    the two is nonzero (ADR-0011)."""

    causing_command_sequence: int = 0
    instrument_id: int = 0
    participant_id: int = 0
    client_order_id: int = 0
    order_id: int = 0
    reason: RejectReason = RejectReason.MALFORMED_MESSAGE


@dataclass(frozen=True, slots=True)
class OrderModifiedEvent:
    causing_command_sequence: int = 0
    order_id: int = 0
    new_remaining_units: int = 0
    cancelled_quantity_delta_units: int = 0


@dataclass(frozen=True, slots=True)
class OrderReplacedEvent:
    causing_command_sequence: int = 0
    old_order_id: int = 0
    new_order_id: int = 0
    instrument_id: int = 0
    participant_id: int = 0
    client_order_id: int = 0
    side: Side = Side.BUY
    order_type: OrderType = OrderType.LIMIT
    price_units: int = 0
    quantity_units: int = 0  # original_quantity of the replacement


@dataclass(frozen=True, slots=True)
class TradeEvent:
    causing_command_sequence: int = 0
    instrument_id: int = 0
    price_units: int = 0
    quantity_units: int = 0
    maker_order_id: int = 0
    taker_order_id: int = 0
    maker_participant_id: int = 0
    taker_participant_id: int = 0
    taker_side: Side = Side.BUY


@dataclass(frozen=True, slots=True)
class OrderTerminatedEvent:
    causing_command_sequence: int = 0
    order_id: int = 0
    reason: TerminationReason = TerminationReason.FILLED
    cancelled_quantity_delta_units: int = 0


# --------------------------------------------------------------- wire primitives


def _put_u8(parts: list[bytes], value: int) -> None:
    parts.append(_U8.pack(value))


def _put_u32(parts: list[bytes], value: int) -> None:
    parts.append(_U32.pack(value))


def _put_u64(parts: list[bytes], value: int) -> None:
    parts.append(_U64.pack(value))


def _put_i64(parts: list[bytes], value: int) -> None:
    parts.append(_I64.pack(value))


def _take_u8(data: bytes, offset: int) -> tuple[int, int]:
    if offset + 1 > len(data):
        raise ExchangeMessageDecodeError("message ends before its declared fields do")
    (value,) = _U8.unpack_from(data, offset)
    return value, offset + 1


def _take_u32(data: bytes, offset: int) -> tuple[int, int]:
    if offset + 4 > len(data):
        raise ExchangeMessageDecodeError("message ends before its declared fields do")
    (value,) = _U32.unpack_from(data, offset)
    return value, offset + 4


def _take_u64(data: bytes, offset: int) -> tuple[int, int]:
    if offset + 8 > len(data):
        raise ExchangeMessageDecodeError("message ends before its declared fields do")
    (value,) = _U64.unpack_from(data, offset)
    return value, offset + 8


def _take_i64(data: bytes, offset: int) -> tuple[int, int]:
    if offset + 8 > len(data):
        raise ExchangeMessageDecodeError("message ends before its declared fields do")
    (value,) = _I64.unpack_from(data, offset)
    return value, offset + 8


def _take_enum[EnumT: IntEnum](kind: type[EnumT], data: bytes, offset: int) -> tuple[EnumT, int]:
    raw, offset = _take_u8(data, offset)
    try:
        return kind(raw), offset
    except ValueError:
        raise ExchangeMessageDecodeError(f"{raw} is not a known {kind.__name__}") from None


def _finish(data: bytes, offset: int) -> None:
    if offset != len(data):
        raise ExchangeMessageDecodeError("message carries bytes beyond its declared payload")


# --------------------------------------------------------------------- commands


def encode_new_order(command: NewOrderCommand) -> bytes:
    parts: list[bytes] = []
    _put_u32(parts, command.instrument_id)
    _put_u64(parts, command.participant_id)
    _put_u64(parts, command.client_order_id)
    _put_u8(parts, int(command.side))
    _put_u8(parts, int(command.order_type))
    _put_i64(parts, command.price_units)
    _put_i64(parts, command.quantity_units)
    return b"".join(parts)


def decode_new_order(data: bytes) -> NewOrderCommand:
    offset = 0
    instrument_id, offset = _take_u32(data, offset)
    participant_id, offset = _take_u64(data, offset)
    client_order_id, offset = _take_u64(data, offset)
    side, offset = _take_enum(Side, data, offset)
    order_type, offset = _take_enum(OrderType, data, offset)
    price_units, offset = _take_i64(data, offset)
    quantity_units, offset = _take_i64(data, offset)
    _finish(data, offset)
    return NewOrderCommand(instrument_id, participant_id, client_order_id, side, order_type, price_units,
                            quantity_units)


def encode_cancel_order(command: CancelOrderCommand) -> bytes:
    parts: list[bytes] = []
    _put_u32(parts, command.instrument_id)
    _put_u64(parts, command.participant_id)
    _put_u64(parts, command.order_id)
    return b"".join(parts)


def decode_cancel_order(data: bytes) -> CancelOrderCommand:
    offset = 0
    instrument_id, offset = _take_u32(data, offset)
    participant_id, offset = _take_u64(data, offset)
    order_id, offset = _take_u64(data, offset)
    _finish(data, offset)
    return CancelOrderCommand(instrument_id, participant_id, order_id)


def encode_modify_order(command: ModifyOrderCommand) -> bytes:
    parts: list[bytes] = []
    _put_u32(parts, command.instrument_id)
    _put_u64(parts, command.participant_id)
    _put_u64(parts, command.order_id)
    _put_i64(parts, command.new_price_units)
    _put_i64(parts, command.new_quantity_units)
    return b"".join(parts)


def decode_modify_order(data: bytes) -> ModifyOrderCommand:
    offset = 0
    instrument_id, offset = _take_u32(data, offset)
    participant_id, offset = _take_u64(data, offset)
    order_id, offset = _take_u64(data, offset)
    new_price_units, offset = _take_i64(data, offset)
    new_quantity_units, offset = _take_i64(data, offset)
    _finish(data, offset)
    return ModifyOrderCommand(instrument_id, participant_id, order_id, new_price_units, new_quantity_units)


# ----------------------------------------------------------------------- events


def encode_order_accepted(event: OrderAcceptedEvent) -> bytes:
    parts: list[bytes] = []
    _put_u64(parts, event.causing_command_sequence)
    _put_u64(parts, event.order_id)
    _put_u32(parts, event.instrument_id)
    _put_u64(parts, event.participant_id)
    _put_u64(parts, event.client_order_id)
    _put_u8(parts, int(event.side))
    _put_u8(parts, int(event.order_type))
    _put_i64(parts, event.price_units)
    _put_i64(parts, event.quantity_units)
    return b"".join(parts)


def decode_order_accepted(data: bytes) -> OrderAcceptedEvent:
    offset = 0
    causing_command_sequence, offset = _take_u64(data, offset)
    order_id, offset = _take_u64(data, offset)
    instrument_id, offset = _take_u32(data, offset)
    participant_id, offset = _take_u64(data, offset)
    client_order_id, offset = _take_u64(data, offset)
    side, offset = _take_enum(Side, data, offset)
    order_type, offset = _take_enum(OrderType, data, offset)
    price_units, offset = _take_i64(data, offset)
    quantity_units, offset = _take_i64(data, offset)
    _finish(data, offset)
    return OrderAcceptedEvent(causing_command_sequence, order_id, instrument_id, participant_id,
                               client_order_id, side, order_type, price_units, quantity_units)


def encode_order_rejected(event: OrderRejectedEvent) -> bytes:
    parts: list[bytes] = []
    _put_u64(parts, event.causing_command_sequence)
    _put_u32(parts, event.instrument_id)
    _put_u64(parts, event.participant_id)
    _put_u64(parts, event.client_order_id)
    _put_u64(parts, event.order_id)
    _put_u8(parts, int(event.reason))
    return b"".join(parts)


def decode_order_rejected(data: bytes) -> OrderRejectedEvent:
    offset = 0
    causing_command_sequence, offset = _take_u64(data, offset)
    instrument_id, offset = _take_u32(data, offset)
    participant_id, offset = _take_u64(data, offset)
    client_order_id, offset = _take_u64(data, offset)
    order_id, offset = _take_u64(data, offset)
    reason, offset = _take_enum(RejectReason, data, offset)
    _finish(data, offset)
    return OrderRejectedEvent(causing_command_sequence, instrument_id, participant_id, client_order_id,
                               order_id, reason)


def encode_order_modified(event: OrderModifiedEvent) -> bytes:
    parts: list[bytes] = []
    _put_u64(parts, event.causing_command_sequence)
    _put_u64(parts, event.order_id)
    _put_i64(parts, event.new_remaining_units)
    _put_i64(parts, event.cancelled_quantity_delta_units)
    return b"".join(parts)


def decode_order_modified(data: bytes) -> OrderModifiedEvent:
    offset = 0
    causing_command_sequence, offset = _take_u64(data, offset)
    order_id, offset = _take_u64(data, offset)
    new_remaining_units, offset = _take_i64(data, offset)
    cancelled_quantity_delta_units, offset = _take_i64(data, offset)
    _finish(data, offset)
    return OrderModifiedEvent(causing_command_sequence, order_id, new_remaining_units,
                               cancelled_quantity_delta_units)


def encode_order_replaced(event: OrderReplacedEvent) -> bytes:
    parts: list[bytes] = []
    _put_u64(parts, event.causing_command_sequence)
    _put_u64(parts, event.old_order_id)
    _put_u64(parts, event.new_order_id)
    _put_u32(parts, event.instrument_id)
    _put_u64(parts, event.participant_id)
    _put_u64(parts, event.client_order_id)
    _put_u8(parts, int(event.side))
    _put_u8(parts, int(event.order_type))
    _put_i64(parts, event.price_units)
    _put_i64(parts, event.quantity_units)
    return b"".join(parts)


def decode_order_replaced(data: bytes) -> OrderReplacedEvent:
    offset = 0
    causing_command_sequence, offset = _take_u64(data, offset)
    old_order_id, offset = _take_u64(data, offset)
    new_order_id, offset = _take_u64(data, offset)
    instrument_id, offset = _take_u32(data, offset)
    participant_id, offset = _take_u64(data, offset)
    client_order_id, offset = _take_u64(data, offset)
    side, offset = _take_enum(Side, data, offset)
    order_type, offset = _take_enum(OrderType, data, offset)
    price_units, offset = _take_i64(data, offset)
    quantity_units, offset = _take_i64(data, offset)
    _finish(data, offset)
    return OrderReplacedEvent(causing_command_sequence, old_order_id, new_order_id, instrument_id,
                               participant_id, client_order_id, side, order_type, price_units,
                               quantity_units)


def encode_trade(event: TradeEvent) -> bytes:
    parts: list[bytes] = []
    _put_u64(parts, event.causing_command_sequence)
    _put_u32(parts, event.instrument_id)
    _put_i64(parts, event.price_units)
    _put_i64(parts, event.quantity_units)
    _put_u64(parts, event.maker_order_id)
    _put_u64(parts, event.taker_order_id)
    _put_u64(parts, event.maker_participant_id)
    _put_u64(parts, event.taker_participant_id)
    _put_u8(parts, int(event.taker_side))
    return b"".join(parts)


def decode_trade(data: bytes) -> TradeEvent:
    offset = 0
    causing_command_sequence, offset = _take_u64(data, offset)
    instrument_id, offset = _take_u32(data, offset)
    price_units, offset = _take_i64(data, offset)
    quantity_units, offset = _take_i64(data, offset)
    maker_order_id, offset = _take_u64(data, offset)
    taker_order_id, offset = _take_u64(data, offset)
    maker_participant_id, offset = _take_u64(data, offset)
    taker_participant_id, offset = _take_u64(data, offset)
    taker_side, offset = _take_enum(Side, data, offset)
    _finish(data, offset)
    return TradeEvent(causing_command_sequence, instrument_id, price_units, quantity_units, maker_order_id,
                       taker_order_id, maker_participant_id, taker_participant_id, taker_side)


def encode_order_terminated(event: OrderTerminatedEvent) -> bytes:
    parts: list[bytes] = []
    _put_u64(parts, event.causing_command_sequence)
    _put_u64(parts, event.order_id)
    _put_u8(parts, int(event.reason))
    _put_i64(parts, event.cancelled_quantity_delta_units)
    return b"".join(parts)


def decode_order_terminated(data: bytes) -> OrderTerminatedEvent:
    offset = 0
    causing_command_sequence, offset = _take_u64(data, offset)
    order_id, offset = _take_u64(data, offset)
    reason, offset = _take_enum(TerminationReason, data, offset)
    cancelled_quantity_delta_units, offset = _take_i64(data, offset)
    _finish(data, offset)
    return OrderTerminatedEvent(causing_command_sequence, order_id, reason, cancelled_quantity_delta_units)
