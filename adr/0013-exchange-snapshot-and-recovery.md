# ADR-0013: Exchange snapshot and recovery

- Status: Accepted
- Date: 2026-08-07
- Requirement IDs: AEGIS-005, AEGIS-027, AEGIS-036
- Milestone: M1

## Context

`docs/RECOVERY_CONTRACT.md` registers an M1 obligation: "Exchange-state
recovery test: sequencer position and book state survive a snapshot/restore
cycle, verified by round trip." ADR-0012 established three independent
identifier spaces — `CommandSequence`, `EventSequence`, `OrderId` — each with
its own counter, specifically so that losing one on restore would be
detectable rather than silently reproducing plausible-looking output from the
wrong state. This ADR is the snapshot and recovery design that claim
depended on: it did not exist when ADR-0012 and ADR-0011 were written, so
both note that the design "is recorded in its own ADR when that layer lands"
— this is that ADR.

A round-trip test (`write == restore → write`) is necessary but not
sufficient: it can pass even if restore silently drops a counter, as long as
the dropped counter's *value* happens to still be embedded correctly in the
restored book. The failure mode ADR-0012 was written to make visible is a
counter that diverges only once the restored exchange keeps running — which
a round-trip test never exercises, because it never resumes command
processing after restore.

## Decision

**`ExchangeSnapshot` (`cpp/exchange/state/snapshot.{hpp,cpp}`)** carries a
header — `{snapshot_version, next_command_sequence, next_event_sequence,
next_order_id, last_exchange_time}` — plus every resting order across every
registered instrument, each carrying `order_id`, `instrument_id`,
`participant_id`, `client_order_id`, `side`, `order_type`, `price_units`,
`original_quantity`, `cumulative_filled`, `cancelled_quantity`, `remaining`
and its `priority` (the `CommandSequence` value it was assigned from).

**Canonical order is `(instrument_id, price_units, priority)`, all
ascending.** The plan of record states "price units ascending, then priority
ascending," written from the frame of one instrument; extending it with
`instrument_id` as the leading key is the natural generalization to an
`ExchangeNode` that can register more than one, and it is not an arbitrary
choice: `OrderBook::add` always appends to the tail of its level's FIFO queue
regardless of the descriptor's own `priority` field (`tests/cpp/unit/test_book_invariants.cpp`
already exercises this to build corrupted fixtures). Replaying restored
orders into `OrderBook::add` in any order other than priority-ascending
*within* each `(instrument_id, price_units)` group would silently
reconstruct the wrong FIFO queue — a book that looks structurally valid
(every `check_invariants` predicate still holds) but resolves ties in the
wrong arrival order. Sorting by the full three-key tuple guarantees the
priority-ascending sub-property holds within every group without needing to
group explicitly.

**The live `(ParticipantId, ClientOrderId) → OrderId` map is never
snapshotted.** It is exactly the currently-open subset of what
`ExchangeSnapshot.orders` already contains, so persisting it separately would
create a second copy that restore could make disagree with the book. Restore
rebuilds it by replaying every restored order through the existing
`OrderBook::add`, which already maintains this map as a side effect
(landed slice 1) — the same reasoning ADR-0011 §"Retention" gives for why
this map is not itself part of the snapshot.

**Byte-stable encoding reuses `cpp/events/wire.hpp`.** Fixed field order,
fixed-width little-endian, no floating point — the same rules
`cpp/events/exchange_messages.cpp` already follows, so the snapshot codec is
a sixth wire format built on the same primitives rather than a bespoke one
(`wire.hpp`'s own header comment already names this as its purpose).

**`read_snapshot` refuses two distinct failure modes with two distinct
reasons**, mirroring `events::DecodeResult`/`DecodeError`
(`cpp/events/envelope.hpp`):

- `kUnknownVersion` — `snapshot_version` is not `kSnapshotVersion` (1).
- `kCounterInconsistent` — `next_order_id` is at or below some restored
  order's `order_id`, or `next_command_sequence` is at or below some restored
  order's `priority`. Both are the exact failure ADR-0012's three-counter
  design exists to make detectable: a snapshot whose header counters do not
  dominate its own contents would, if accepted, hand out an `OrderId` or a
  `CommandSequence`/`Priority` that collides with a value already live in the
  restored book.

Both refusals ship a negative fixture in `test_snapshot_roundtrip.cpp` rather
than only a positive path, matching the house rule that every gate carries
one.

**Restoring a running exchange is orchestration, not state-layer code.**
`cpp-exchange-state` may depend on `cpp-common`, `cpp-events`,
`cpp-exchange-sequencer` and `cpp-exchange-order-book` only — never on
`cpp-exchange-app` (`configs/architecture_rules.yaml`). So `snapshot.{hpp,cpp}`
exposes `capture_snapshot` (reads a `Sequencer`, an `EventLog`, a
`next_order_id` value and a list of `OrderBook*`) and `restore_orders_into`
(replays one instrument's records into one already-constructed `OrderBook`)
as free functions over already-public types, and never references
`ExchangeNode`. The composition root — `ExchangeNode`, which is the only
layer permitted to see sequencer, book, matching and state at once
(ADR-0012) — gains the restore-aware constructor and the `next_order_id()`
accessor `capture_snapshot` needs, and is what a test (or, in slice 10, the
replay CLI) actually calls.

**Acceptance is continuation equality, not round trip alone.** A run split
as *first half → snapshot → restore into a fresh `ExchangeNode` → second
half* must emit byte-identical canonical output — including every
`EventSequence` and every `OrderId` assigned during the second half — to the
same command stream run uninterrupted through one `ExchangeNode`. This is
the test ADR-0012 promised: a counter silently dropped or reset on restore
does not fail a `write == restore → write` check (the restored snapshot's
own bytes are internally consistent either way) but does fail continuation
equality, because the second half then assigns colliding or non-monotonic
identifiers. `test_snapshot_roundtrip.cpp` asserts both — `write ==
restore → write` proves the codec is faithful; continuation equality proves
restore actually reconstructs the exchange's runtime position, not just a
byte-identical description of it.

## Alternatives considered

- **Persist the live client-id map alongside the orders.** Rejected: two
  copies of the same fact that restore could make disagree, for no benefit —
  `OrderBook::add` already derives it deterministically from the same order
  list the snapshot already carries.
- **A single global "next id" counter shared by all three spaces.**
  Rejected in ADR-0012 already; this ADR's counter-consistency refusal is the
  concrete mechanism that would have caught the failure ADR-0012 argued
  against in the abstract.
- **Snapshot each `OrderBook` independently, keyed by instrument, with no
  shared header.** Rejected: `next_command_sequence`, `next_event_sequence`
  and `next_order_id` are each a single exchange-wide counter (ADR-0012), not
  per-instrument, so per-book snapshots would either duplicate the same three
  numbers in every book's snapshot (another two-copies-that-can-disagree
  hazard) or require a second, separate persisted record for them — more
  moving parts than one shared header with a canonically ordered order list
  spanning every instrument.
- **Round-trip test only, no continuation equality.** Rejected: see
  "Acceptance" above — round trip alone cannot observe a dropped counter.

## Consequences

- `cpp/exchange/state/snapshot.{hpp,cpp}` is the only place that knows the
  on-disk/on-wire snapshot layout; `cpp/exchange/app/exchange_node.{hpp,cpp}`
  is the only place that knows how to turn one into a running exchange.
- `MatchingEngine` gains a restore-aware constructor
  (`MatchingEngine(const MatchingPolicy&, std::uint64_t next_order_id)`) and
  a `next_order_id()` accessor, alongside its existing default-constructing
  one — mirroring the restore-constructor pattern `Sequencer` and `EventLog`
  already established in slice 1.
- A future replay CLI (`aegis_exchange_replay --snapshot-at N
  --restore-from`, slice 10) is a thin wrapper over exactly this
  `capture_snapshot`/`read_snapshot`/`ExchangeNode` restore path — it adds no
  new recovery logic of its own.
- `experiments/plans/M1.md`'s forward reference to ADR-0013 now resolves
  against a real document; the `"0013"` entry in `tools/check_adrs.py`'s
  `DANGLING_REFERENCE_EXEMPTIONS` becomes inert (harmless to leave, since the
  reference no longer needs the exemption to pass) rather than something
  this slice needs to prune — `check_adrs.py`'s own test suite pins that
  exact number as its exemption-mechanism example independent of whether the
  real ADR exists.

## Verification

- `tests/cpp/unit/test_snapshot_roundtrip.cpp` — byte-stable `write_snapshot`;
  `kUnknownVersion` refusal; `kCounterInconsistent` refusal (two fixtures:
  an order-id conflict and a command-sequence/priority conflict); `write ==
  restore → write`; continuation equality including every `EventSequence`
  and every `OrderId` assigned after restore.
- `python3 tools/check_architecture.py` — confirms `cpp-exchange-state` still
  depends on nothing outside `{common, events, sequencer, order_book}` and
  that only `cpp-exchange-app` composes sequencer + book + matching + state.
- `python3 tools/check_adrs.py` — confirms the plan's forward reference to
  ADR-0013 now resolves to a real document.

## Owner approval

Confirmed by the owner per experiments/plans/M1.md §5 (slice 9) and §4.3
(snapshot header and counter-consistency rules).
