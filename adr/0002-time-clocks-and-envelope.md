# ADR-0002: Time domains, injected clocks and the message envelope

- Status: Accepted
- Date: 2026-08-06
- Requirement IDs: AEGIS-005, AEGIS-232, AEGIS-227
- Milestone: M0

## Context

`docs/ARCHITECTURE.md` names seven clocks — event, receive, decision, submit,
exchange, acknowledgement/fill and monotonic — and forbids deriving latency from
wall-clock stamps without documenting synchronisation.

All seven are 64-bit nanosecond counts. Nothing about that representation
prevents `ack_time - monotonic_time`, and the result is a plausible-looking
number rather than an error. That is precisely how a latency figure computed
across two unrelated clocks reaches a benchmark report, and no test detects it
because the number is the right order of magnitude.

The same argument applies to the wire format. If M1 invents its own framing,
M2's virtual clock, M3's latency model and M8's latency attribution each
re-derive it, and retrofitting afterwards means regenerating every golden file
recorded in between.

## Decision

**Time.** Each domain is a distinct type (`cpp/common/time.hpp`,
`python/common/clock.py`). Only same-domain subtraction compiles in C++ and only
same-domain subtraction succeeds in Python; the Python error names both domains
and points at `elapsed()`. `serialize_nanos` is constrained so a `MonotonicTime`
cannot enter a persisted or hashed record: its origin is unspecified per process,
so a recorded one replays to a different value.

**Clocks.** `WallClock` and `SteadyClock` are interfaces with system and manual
implementations, always constructor-injected. There is no global clock and no
`default_clock()`: a global would be reachable from every book partition, which
is the hidden shared state AEGIS-047 exists to prevent. `ManualSteadyClock`
cannot move backwards, because a negative latency must never be
indistinguishable from a real bug.

**Envelope.** `cpp/events/envelope.hpp` and `python/common/envelope.py` define
one header carried by every runtime boundary: schema version, message type,
sequence, stream id, event time, producer id, experiment id, correlation id, and
an opaque length-prefixed payload. The encoding is canonical — one value has
exactly one byte representation:

- fixed field order, no optional fields, no padding;
- fixed-width little-endian integers, never native byte order;
- no floating point anywhere;
- length-prefixed strings;
- no map or set iteration order, no locale.

Message type numbers are permanent. A recorded stream outlives the code that
wrote it, so a reused number would decode old recordings into the wrong type.
M0 defines only the platform framing type; 1..999 is reserved for exchange
messages (M1) and 1000..1999 for participant messages (M3).

Decode returns a result rather than throwing: a malformed message on a feed is
an expected event a feed handler counts and continues past. Unknown schema
version, unknown message type, truncation, length overflow and trailing bytes
are distinct, named outcomes.

**Deferred:** the price representation. Fixed-point scale, tick handling and
rounding are decided in M1 with the order book that uses them; guessing now
would bake a choice into a wire format before anything constrains it.

## Alternatives considered

**A single `Timestamp` type with a runtime domain tag.** Rejected: it moves the
error to runtime, which for a latency computation means it is never detected —
the wrong answer is still a number.

**Strong types by convention (naming, comments).** Rejected: this is exactly
what `docs/ARCHITECTURE.md` already says, and the decision exists because saying
it is not sufficient.

**Protobuf or FlatBuffers for the envelope.** Rejected for M0: both offer
encoding freedom (field ordering, optional presence, varint encoding) that would
have to be constrained back down to get canonical bytes, and both add a
build-time dependency to a header that has one integer field group.

**Deferring the envelope to M1.** Considered seriously. Rejected because the
retrofit cost lands on the golden files M1 produces, which is the worst moment.

## Consequences

- Crossing a clock domain is explicit at every call site.
- Every component that needs the time takes a clock, which is more constructor
  parameters and the reason a determinism fixture can be byte-stable.
- The envelope carries three length-prefixed strings on every message; this is
  M0's correctness-first choice, and M8 may revisit the representation with a
  benchmark and an equivalence proof, not before.
- A wire-format change is a schema-version change with an ADR, not an edit.

## Verification

- `tests/cpp/unit/test_time.cpp` asserts the compile-time rejections with
  `static_assert`; a runtime test could not express "this must not compile".
- `tests/unit/test_clock.py` asserts the Python runtime refusals.
- `tests/property/test_envelope_encoding.py` checks round-trip fidelity,
  encoding-is-a-function and encoding-is-injective over generated inputs.
- `tests/unit/fixtures/envelope/golden.json` pins both encoders to committed
  bytes; `tests/integration/test_bindings_roundtrip.py` compares them live.

## Owner approval

Recorded in the approved M0 plan (`experiments/plans/M0.md`, P5 and Part 4).
