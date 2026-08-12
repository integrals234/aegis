# ADR-0020: Participant layering, composition root and market-data wire vocabulary

- Status: Accepted
- Date: 2026-08-12
- Requirement IDs: AEGIS-064, AEGIS-065, AEGIS-098, AEGIS-099, AEGIS-100, AEGIS-107, AEGIS-108, AEGIS-118
- Milestone: M3

## Context

M3 populates six layers `configs/architecture_rules.yaml` already dated to
this milestone (`cpp-exchange-market-data`, `cpp-participant-feed-handler`,
`cpp-participant-book-builder`, `cpp-statistics`, `cpp-participant-oms`,
`cpp-participant-portfolio`), but three structural gaps stood in the way of
starting:

1. **No composition root.** Nothing may legally see feed handler, book
   builder, statistics, OMS and portfolio at once — `cpp-exchange-app` is the
   precedent (ADR-0012: "nothing else may legally see all of sequencer, book,
   matching and state at once"), and the participant side has no equivalent.
   AEGIS-109/114 ("integration tests reconcile fills and positions") and
   AEGIS-237 (participant-state recovery) both need exactly that view.
2. **No wire vocabulary for book depth.** `cpp/events` carries order and trade
   events (ADR-0009), which the participant already decodes without depending
   on the exchange layer. Nothing carries book *depth* — a snapshot or an
   incremental update — and AEGIS-064/065 need one.
3. **`cpp-statistics` was declared with a participant-domain dependency.**
   `configs/architecture_rules.yaml` originally gave it
   `may_depend_on: [cpp-common, cpp-participant-book-builder]`. AEGIS-098–107
   describe a generic numeric library (rolling mean, variance, covariance,
   z-score, EW statistics), and binding it to book types would make it
   unusable by M4's strategy layer, M5's risk layer, M6's attribution and M7's
   decision intelligence without inheriting participant concepts they do not
   need.

All three were identified during M3 planning and are the subject of the
owner-approved, exact-path `m3-architecture-transition` entry in
`configs/governance/policy.yaml`, which enumerates exactly the changes this
ADR records.

## Decision

**Market-data messages live in `cpp/events`, in the exchange's numeric band.**
`cpp/events/market_data_messages.hpp` defines `BookSnapshotEvent` (a complete,
self-sufficient book state as a flat list of per-order entries in canonical
FIFO order) and `BookDeltaEvent` (one incremental change). `MessageType`
gains `kBookSnapshot = 20` and `kBookDelta = 21` — permanent numbers in the
`1..999` exchange-side band (`cpp/events/envelope.hpp`), not the `1000..1999`
participant band, because these messages are exchange-**published**. This is
exactly ADR-0009's reasoning applied a second time: the vocabulary lives where
a consumer can decode it without depending on the producer's layer.

**`cpp/exchange/market_data` observes; it does not participate in matching.**
The publisher derives a `BookSnapshotEvent` from a live `OrderBook` by walking
`best_price()`/`next_price_after()` on each side and `orders_at()` per level —
read-only calls already public on `OrderBook`. It derives `BookDeltaEvent`s
from the existing `EmittedEvent` stream (`OrderAccepted`, `OrderModified`,
`OrderReplaced`, `OrderTerminated`) by maintaining its own
`order_id -> (side, price)` map, populated and retired exactly as those events
already announce. `MatchingEngine` is never called by this layer and never
calls it; emitted-event order is untouched.

**A participant composition root: `cpp-participant-app`.** New layer,
`cpp/participant/app`, namespace `aegis::participant::app`, mirroring
`cpp-exchange-app`'s role exactly. It is the only participant layer permitted
to depend on `cpp-replay` — the channel M2's `DeterministicFaultInjector`
reaches the participant through for AEGIS-060/061, without either layer
depending on the other — and it has **no edge to any `cpp-exchange-*`
layer**, ever: the exchange and the participant meet only through versioned
messages, never a production dependency (MASTER_SPEC immutable principle 1).
It hosts `aegis_participant_run`, the deterministic CLI evidence generators
drive, matching `aegis_exchange_replay` (M1) and `aegis_replay_run` (M2).

**`cpp-statistics` is narrowed to `[cpp-common]`.** It implements
AEGIS-098–107 over plain numeric observations (`double`, fixed-size windows)
and knows nothing of books, orders, feeds or participants. Book-derived
observations (a trade price, a fill event, a depth number) are extracted by
whichever participant layer owns that domain concept and handed to
`cpp-statistics` as a bare number; the two meet only inside
`cpp-participant-app`. No compensating `cpp-participant-book-builder ->
cpp-statistics` edge exists either — the dependency was removed, not
reversed.

**`cpp-bindings` gains exactly one new edge: `cpp-statistics`.** AEGIS-107
("Cross-language report is committed") needs the C++ estimators callable from
Python to compare against `python/common/online_stats.py`. No other
`cpp-exchange-*` or `cpp-participant-*` layer is added to `cpp-bindings` — the
binding surface does not widen past what this requirement needs.

**`cpp-exchange-app` gains exactly one new edge: `cpp-exchange-market-data`.**
The publisher is instantiated and driven from the exchange composition root,
alongside the sequencer, book, matching engine and state it already
coordinates.

## Alternatives considered

- **A test binary as the only composition root** — rejected: leaves no
  runnable evidence producer, unlike `aegis_exchange_replay`/`aegis_replay_run`
  in the milestones before this one.
- **Market-data types under `cpp/exchange/market_data` itself** — rejected: a
  participant decoding them would then depend on `cpp-exchange-market-data`
  directly, reopening exactly the participant→exchange dependency AEGIS-004
  forbids structurally.
- **Widening `cpp-participant-oms.may_depend_on` to reach the book or
  statistics directly** — rejected: weakens the enforced graph for local
  convenience; the OMS declares its own narrow input types instead (ADR-0023).
- **Keeping `cpp-statistics -> cpp-participant-book-builder`, or adding the
  reverse edge** — rejected: either binds a generic numeric library to
  participant-domain types it does not need, defeating reuse at M4–M7.

## Consequences

- `cpp/exchange/market_data`, `cpp/participant/feed_handler`,
  `cpp/participant/book_builder`, `cpp/statistics`, `cpp/participant/oms`,
  `cpp/participant/portfolio` and `cpp/participant/app` are all non-empty from
  this commit, satisfying `check_layer_population` in both directions for
  every M3-dated layer.
- M4's strategy layer, M5's risk layer, M6's attribution and M7's decision
  intelligence can depend on `cpp-statistics` without inheriting participant
  types.
- A future market-data requirement that needs a third message kind extends
  `cpp/events/market_data_messages.hpp` with a new permanent `MessageType`
  number in the same band — no layering change.

## Verification

- `tools/check_architecture.py` passes with the four `architecture_rules.yaml`
  changes this ADR records and no others; the `governance` CI job runs it on
  every PR.
- `tests/cpp/unit/test_market_data_messages.cpp` — snapshot and delta
  encode/decode round-trip, including a truncated-payload failure case.
- `tests/cpp/unit/test_market_data_publisher.cpp` — a snapshot captured from a
  populated `OrderBook` reconstructs the same levels and quantities; deltas
  derived from a sequence of `EmittedEvent`s produce the expected add/modify/
  remove sequence with no `OrderBook` mutation observable from the publisher's
  own state.

## Owner approval

Authorized under the owner-approved M3 plan of record (`experiments/plans/M3.md`)
and the `m3-architecture-transition` exact-path approval in
`configs/governance/policy.yaml` (PR #7), 2026-08-12.
