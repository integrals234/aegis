# ADR-0009: Exchange domain model and wire vocabulary

- Status: Accepted
- Date: 2026-08-07
- Requirement IDs: AEGIS-027, AEGIS-028, AEGIS-029, AEGIS-033, AEGIS-034, AEGIS-035
- Milestone: M1

## Context

M1 needs a permanent vocabulary for the messages the exchange emits and
consumes — new order, cancel, modify, and the events a matching engine
produces — plus a representation for price and quantity that makes tick and
lot validation genuine rather than tautological, and a clock the engine never
reads.

`cpp/events/envelope.hpp` already reserves `MessageType` 1..999 for exchange
domain messages and states the canonical-encoding rules (fixed field order,
fixed-width little-endian, length-prefixed strings, no floating point, no map
iteration order). It defines none of them. AEGIS-005 requires byte-identical
canonical output for identical input, which only holds if every value the
engine can produce has exactly one wire representation, and if the engine's
own state never depends on anything outside its input — a wall clock, a random
generator, hash-table iteration order.

## Decision

**Domain messages live in `cpp/events`, not in the exchange.** This keeps
`cpp-events` message-shape-only and lets a later consumer (the M3 participant)
decode exchange events without depending on the exchange layer at all —
AEGIS-004 made structural. `cpp/events/exchange_messages.{hpp,cpp}` adds
permanent `MessageType` numbers in the reserved band:

- Commands: `kNewOrder=1`, `kCancelOrder=2`, `kModifyOrder=3`.
- Events: `kOrderAccepted=10`, `kOrderRejected=11`, `kOrderModified=12`,
  `kOrderReplaced=13`, `kTrade=14`, `kOrderTerminated=15`.

Numbers are permanent: a retired type keeps its number forever, because a
recorded stream outlives the code that wrote it. `is_known_message_type()` is
extended to accept them; its existing unit test is updated rather than
replaced, so `kUnspecified` staying valid is still checked.

**No floating point anywhere in engine state, canonical encodings or
snapshots.** Price and quantity are `PriceUnits` and `QuantityUnits` — strong,
non-implicitly-convertible `int64_t` wrappers holding the smallest
representable unit. `InstrumentSpec` declares the grid on top of them
(`price_floor_units`, `price_ceiling_units`, `tick_size_units`,
`min_quantity_units`, `max_quantity_units`, `lot_size_units`), so an accepted
order is provably on-tick and on-lot by construction rather than by
convention. M1 attaches no currency, multiplier or display scale to a price
unit — that is AEGIS-011 (M2) instrument metadata — and computes no notional,
so there is no `price × quantity` product to overflow.

**Three separate identifier spaces** — `CommandSequence`, `EventSequence`,
`OrderId` — are distinct strong types, never derived from one another. Detail
and rationale are in ADR-0012, which owns sequencing.

**The engine reads no clock.** `ExchangeTime = max(previous_exchange_time,
command.event_time)`, derived purely from input.
`aegis::common::MonotonicTime` is never serialized — `serialize_nanos` already
refuses it at compile time (`cpp/common/time.hpp`) — and appears only in the
M1 benchmark driver, never in engine state.

**The Python peer is symmetric.** `python/common/exchange_messages.py`
implements both `encode` and `decode` for every command and event, mirroring
`python/common/envelope.py`. This keeps the hypothesis round-trip pattern of
`tests/property/test_envelope_encoding.py` legitimate at the exchange-message
level. A C++-authored golden hex fixture is a separate, second check: it
verifies the two languages agree on one canonical byte string, which a
round-trip test alone cannot — a symmetric bug in both encoders would pass a
round trip and still disagree with the other language.

## Alternatives considered

- **Domain messages inside `cpp/exchange`.** Rejected: it would make the M3
  participant depend on the exchange layer just to decode what the exchange
  published, which is exactly the coupling AEGIS-004 forbids structurally.
- **`double` for price.** Rejected: the same double prints and rounds
  differently under different libc and compiler flags, which makes a replay
  hash a property of the toolchain rather than of the engine — precisely what
  `cpp/events/envelope.hpp` already rules out for the envelope itself.
- **Golden hex fixture only, no symmetric Python encode.** Rejected: it proves
  Python can decode what C++ wrote, never that a Python-originated message
  round-trips, so a Python encoding bug would only surface later, in an M2+
  producer nobody suspects.

## Consequences

- Every M1 message type is permanent; a future milestone cannot renumber or
  reuse a value even if the type is retired.
- Instrument metadata (currency, multiplier, display scale) is explicitly out
  of scope for M1 and must not be added to `PriceUnits`/`QuantityUnits`
  without a new ADR, because that would change the wire format after fixtures
  exist.
- `python/common/exchange_messages.py` must be kept in lockstep with
  `cpp/events/exchange_messages.{hpp,cpp}`; the golden-hex fixture and the
  hypothesis round-trip test both fail if the two drift.

## Verification

- `tests/cpp/unit/test_exchange_messages.cpp` — encode/decode round trip,
  unknown-type rejection, permanent numbering, golden hex shared with Python.
- `tests/unit/test_exchange_message_codec.py` — Python decodes the C++ golden
  hex; both languages agree byte for byte.
- `tests/property/test_exchange_codec.py` — hypothesis round trip over the
  symmetric Python peer.
- `tests/unit/test_exchange_determinism_lint.py` — greps `cpp/exchange/**` for
  clock and RNG reads.

## Owner approval

Confirmed by the owner per experiments/plans/M1.md §11.3 (semantic choices of
§3, §4.4).
