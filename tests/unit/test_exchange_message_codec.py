"""ADR-0009 — the Python encoder/decoder must agree with C++ byte for byte.

``tests/cpp/unit/test_exchange_messages.cpp`` asserts the same fixture. This is
the payload-level peer of ``tests/unit/test_envelope_golden.py``: both
languages are held to one committed byte string, so a change made to one
encoder and not the other fails a test instead of surfacing as a corrupt
recording after the fact.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from common import exchange_messages as em

pytestmark = pytest.mark.unit

GOLDEN = json.loads(
    (Path(__file__).parent / "fixtures/exchange_messages/golden.json").read_text(encoding="utf-8")
)
CASES = GOLDEN["cases"]

_BUILDERS = {
    "kNewOrder": lambda f: em.NewOrderCommand(
        f["instrument_id"], f["participant_id"], f["client_order_id"], em.Side[f["side"]],
        em.OrderType[f["order_type"]], f["price_units"], f["quantity_units"]
    ),
    "kCancelOrder": lambda f: em.CancelOrderCommand(f["instrument_id"], f["participant_id"], f["order_id"]),
    "kModifyOrder": lambda f: em.ModifyOrderCommand(
        f["instrument_id"], f["participant_id"], f["order_id"], f["new_price_units"],
        f["new_quantity_units"]
    ),
    "kOrderAccepted": lambda f: em.OrderAcceptedEvent(
        f["causing_command_sequence"], f["order_id"], f["instrument_id"], f["participant_id"],
        f["client_order_id"], em.Side[f["side"]], em.OrderType[f["order_type"]], f["price_units"],
        f["quantity_units"]
    ),
    "kOrderRejected": lambda f: em.OrderRejectedEvent(
        f["causing_command_sequence"], f["instrument_id"], f["participant_id"], f["client_order_id"],
        f["order_id"], em.RejectReason[f["reason"]]
    ),
    "kOrderModified": lambda f: em.OrderModifiedEvent(
        f["causing_command_sequence"], f["order_id"], f["new_remaining_units"],
        f["cancelled_quantity_delta_units"]
    ),
    "kOrderReplaced": lambda f: em.OrderReplacedEvent(
        f["causing_command_sequence"], f["old_order_id"], f["new_order_id"], f["instrument_id"],
        f["participant_id"], f["client_order_id"], em.Side[f["side"]], em.OrderType[f["order_type"]],
        f["price_units"], f["quantity_units"]
    ),
    "kTrade": lambda f: em.TradeEvent(
        f["causing_command_sequence"], f["instrument_id"], f["price_units"], f["quantity_units"],
        f["maker_order_id"], f["taker_order_id"], f["maker_participant_id"], f["taker_participant_id"],
        em.Side[f["taker_side"]]
    ),
    "kOrderTerminated": lambda f: em.OrderTerminatedEvent(
        f["causing_command_sequence"], f["order_id"], em.TerminationReason[f["reason"]],
        f["cancelled_quantity_delta_units"]
    ),
}

_ENCODERS = {
    "kNewOrder": em.encode_new_order,
    "kCancelOrder": em.encode_cancel_order,
    "kModifyOrder": em.encode_modify_order,
    "kOrderAccepted": em.encode_order_accepted,
    "kOrderRejected": em.encode_order_rejected,
    "kOrderModified": em.encode_order_modified,
    "kOrderReplaced": em.encode_order_replaced,
    "kTrade": em.encode_trade,
    "kOrderTerminated": em.encode_order_terminated,
}

_DECODERS = {
    "kNewOrder": em.decode_new_order,
    "kCancelOrder": em.decode_cancel_order,
    "kModifyOrder": em.decode_modify_order,
    "kOrderAccepted": em.decode_order_accepted,
    "kOrderRejected": em.decode_order_rejected,
    "kOrderModified": em.decode_order_modified,
    "kOrderReplaced": em.decode_order_replaced,
    "kTrade": em.decode_trade,
    "kOrderTerminated": em.decode_order_terminated,
}


@pytest.mark.parametrize("case", CASES, ids=[c["name"] for c in CASES])
def test_encoder_matches_the_golden_bytes_shared_with_cpp(case):
    message_type = case["message_type"]
    value = _BUILDERS[message_type](case["fields"])
    assert _ENCODERS[message_type](value).hex() == case["encoded_hex"]


@pytest.mark.parametrize("case", CASES, ids=[c["name"] for c in CASES])
def test_golden_bytes_decode_back_to_the_case(case):
    message_type = case["message_type"]
    expected = _BUILDERS[message_type](case["fields"])
    decoded = _DECODERS[message_type](bytes.fromhex(case["encoded_hex"]))
    assert decoded == expected


def test_every_message_type_in_the_fixture_is_covered():
    covered = {case["message_type"] for case in CASES}
    assert covered == set(_BUILDERS)


def test_truncated_payload_fails_to_decode():
    command = em.CancelOrderCommand(1, 2, 3)
    encoded = em.encode_cancel_order(command)
    with pytest.raises(em.ExchangeMessageDecodeError):
        em.decode_cancel_order(encoded[:-1])


def test_trailing_bytes_fail_to_decode():
    command = em.CancelOrderCommand(1, 2, 3)
    encoded = em.encode_cancel_order(command) + b"\x00"
    with pytest.raises(em.ExchangeMessageDecodeError):
        em.decode_cancel_order(encoded)


def test_unknown_reject_reason_fails_to_decode():
    event = em.OrderRejectedEvent(1, 2, 3, 4, 0, em.RejectReason.MALFORMED_MESSAGE)
    encoded = bytearray(em.encode_order_rejected(event))
    encoded[-1] = 200  # not a defined RejectReason
    with pytest.raises(em.ExchangeMessageDecodeError):
        em.decode_order_rejected(bytes(encoded))
