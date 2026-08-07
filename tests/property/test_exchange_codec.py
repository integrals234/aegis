"""ADR-0009 — exchange message canonical encoding invariants.

The exchange-message peer of ``tests/property/test_envelope_encoding.py``:
``decode(encode(x)) == x`` for every generated command and event, and encoding
is a function of its input. This is what makes the golden-hex cross-language
check (``tests/unit/test_exchange_message_codec.py``) trustworthy rather than
a single lucky case — if round-tripping failed for some input, a replay hash
would stop measuring the engine and start measuring the encoder.
"""

from __future__ import annotations

import pytest
from common import exchange_messages as em
from hypothesis import given, settings
from hypothesis import strategies as st

pytestmark = pytest.mark.property

uint32 = st.integers(min_value=0, max_value=2**32 - 1)
uint64 = st.integers(min_value=0, max_value=2**64 - 1)
int64 = st.integers(min_value=-(2**63), max_value=2**63 - 1)
sides = st.sampled_from(list(em.Side))
order_types = st.sampled_from(list(em.OrderType))
reject_reasons = st.sampled_from(list(em.RejectReason))
termination_reasons = st.sampled_from(list(em.TerminationReason))

new_orders = st.builds(
    em.NewOrderCommand,
    instrument_id=uint32, participant_id=uint64, client_order_id=uint64,
    side=sides, order_type=order_types, price_units=int64, quantity_units=int64,
)
cancel_orders = st.builds(
    em.CancelOrderCommand, instrument_id=uint32, participant_id=uint64, order_id=uint64
)
modify_orders = st.builds(
    em.ModifyOrderCommand,
    instrument_id=uint32, participant_id=uint64, order_id=uint64,
    new_price_units=int64, new_quantity_units=int64,
)
order_accepted = st.builds(
    em.OrderAcceptedEvent,
    causing_command_sequence=uint64, order_id=uint64, instrument_id=uint32,
    participant_id=uint64, client_order_id=uint64, side=sides, order_type=order_types,
    price_units=int64, quantity_units=int64,
)
order_rejected = st.builds(
    em.OrderRejectedEvent,
    causing_command_sequence=uint64, instrument_id=uint32, participant_id=uint64,
    client_order_id=uint64, reason=reject_reasons,
)
order_modified = st.builds(
    em.OrderModifiedEvent,
    causing_command_sequence=uint64, order_id=uint64,
    new_remaining_units=int64, cancelled_quantity_delta_units=int64,
)
order_replaced = st.builds(
    em.OrderReplacedEvent,
    causing_command_sequence=uint64, old_order_id=uint64, new_order_id=uint64,
    instrument_id=uint32, participant_id=uint64, client_order_id=uint64,
    side=sides, order_type=order_types, price_units=int64, quantity_units=int64,
)
trades = st.builds(
    em.TradeEvent,
    causing_command_sequence=uint64, instrument_id=uint32, price_units=int64,
    quantity_units=int64, maker_order_id=uint64, taker_order_id=uint64,
    maker_participant_id=uint64, taker_participant_id=uint64, taker_side=sides,
)
order_terminated = st.builds(
    em.OrderTerminatedEvent,
    causing_command_sequence=uint64, order_id=uint64, reason=termination_reasons,
    cancelled_quantity_delta_units=int64,
)

CASES = [
    (new_orders, em.encode_new_order, em.decode_new_order),
    (cancel_orders, em.encode_cancel_order, em.decode_cancel_order),
    (modify_orders, em.encode_modify_order, em.decode_modify_order),
    (order_accepted, em.encode_order_accepted, em.decode_order_accepted),
    (order_rejected, em.encode_order_rejected, em.decode_order_rejected),
    (order_modified, em.encode_order_modified, em.decode_order_modified),
    (order_replaced, em.encode_order_replaced, em.decode_order_replaced),
    (trades, em.encode_trade, em.decode_trade),
    (order_terminated, em.encode_order_terminated, em.decode_order_terminated),
]


@pytest.mark.parametrize("strategy,encode,decode", CASES, ids=[c[1].__name__ for c in CASES])
@settings(max_examples=200)
@given(data=st.data())
def test_round_trip_preserves_every_field(data, strategy, encode, decode):
    value = data.draw(strategy)
    assert decode(encode(value)) == value


@pytest.mark.parametrize("strategy,encode,decode", CASES, ids=[c[1].__name__ for c in CASES])
@settings(max_examples=100)
@given(data=st.data())
def test_encoding_is_a_function(data, strategy, encode, decode):
    value = data.draw(strategy)
    assert encode(value) == encode(value)
