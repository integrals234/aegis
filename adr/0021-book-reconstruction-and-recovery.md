# ADR-0021: Book reconstruction, sequence validation, stale data and gap recovery

- Status: Accepted
- Date: 2026-08-12
- Requirement IDs: AEGIS-066, AEGIS-067, AEGIS-068, AEGIS-069, AEGIS-070, AEGIS-060, AEGIS-061
- Milestone: M3

## Context

`cpp/participant/feed_handler` and `cpp/participant/book_builder` are the
participant-side consumer M2's fault injector always needed but did not have:
ADR-0019 states plainly that AEGIS-060's "stale-data response" and AEGIS-061's
"recovery" both "need a participant-side consumer that does not exist before
M3." That consumer has to decide three things: whether an incoming sequence
number is legal, whether the data it is looking at is too old to trust, and
how to get back to a known-good state after a gap.

A second question this ADR answers: how much of the reconstructed book must
be order-level. AEGIS-066 asks for order-level reconstruction "when data
permits"; AEGIS-067 asks for price-level reconstruction "when only aggregated
depth exists." A design that hard-codes one or the other cannot honestly claim
both requirements.

## Decision

**Sequence diagnostics are a pure function of the observed sequence stream,
owned by `cpp-participant-feed-handler`.** `SequenceTracker::observe(uint64_t)`
compares each `md_sequence` against the last one seen and returns exactly one
of four outcomes: `kOk` (first observation, or exactly one more than the
last), `kDuplicate` (equal to the last), `kGap` (more than one greater), or
`kReset` (less than the last). No outcome mutates any book state — the
tracker's only job is to say what happened, not to react to it.

**Order-level and price-level reconstruction are one type, not two.**
`BookBuilder` maintains an aggregated level map per side
(`std::map<price, quantity>`, keyed to sort bids descending and asks
ascending) as its primary state, plus an `order_id -> {side, price}` map that
is populated only when a delta or snapshot entry actually carries a nonzero
`order_id`. A feed that never supplies order identity (AEGIS-067) drives the
aggregated map alone via `DeltaKind::kPriceLevelSet`; a feed that does
(AEGIS-066) drives both via `kOrderAdded`/`kOrderModified`/`kOrderRemoved`,
and the aggregated map is kept consistent as a side effect of each order-level
change. `BookBuilder` deliberately does not depend on
`cpp-exchange-order-book` (`configs/architecture_rules.yaml`'s
`cpp-participant-book-builder.may_depend_on` is `[cpp-common, cpp-events,
cpp-participant-feed-handler]` only) — its level maps are its own, not a
reuse of the exchange's `LevelIndex`.

**Staleness is a feed/book-level fact, not a risk decision.** `BookBuilder`
tracks the wall-clock time of its last applied message via an injected
`common::WallClock&` (never the system clock, so tests drive it with
`common::ManualClock`, matching every other deterministic component in this
codebase) and a count of consecutive `kGap` diagnostics. `is_stale(now)`
answers true once either configured threshold is crossed. A stale book
**refuses to report a fresh top-of-book** rather than silently continuing to
answer with data it can no longer vouch for — that refusal is AEGIS-060's
"stale-data response" in full: mark state stale, and do not treat stale
output as current. No risk policy, limit, or kill switch is implemented here;
that is AEGIS-120, dated M5.

**Recovery is buffer-then-rebase, using the existing snapshot type.** On
detecting a `kGap`, the caller (via `BookBuilder::begin_recovery()`) starts
buffering subsequent deltas rather than discarding or applying them against a
now-untrustworthy state. A fresh `BookSnapshotEvent` — the same type
AEGIS-064 already defines — replaces the book outright
(`apply_snapshot` clears and rebuilds). Buffered deltas whose `md_sequence` is
greater than the snapshot's are then replayed in order; anything at or below
the snapshot's sequence is discarded as already covered. This is AEGIS-061's
"recovery," and it reuses exactly the mechanism AEGIS-070 ("snapshot
recovery") already needs — one recovery path for both requirements' M3
obligations, not two.

**M2's `DeterministicFaultInjector` is consumed unmodified.**
`cpp-participant-app` drives fault scenarios by feeding a
`replay::DeterministicFaultInjector::apply` result's annotated stream through
the feed handler exactly as an unfaulted stream would be — `kDelayed`
produces a late arrival the staleness clock notices, `kMissing`/`kDuplicated`/
`kSequenceGap` produce the diagnostics `SequenceTracker` already classifies.
No change to `cpp/replay/fault_injection.{hpp,cpp}`.

## Alternatives considered

- **Silent gap tolerance** (apply whatever arrives next regardless of a
  detected gap) — rejected: AEGIS-061 explicitly asks for recovery tests, and
  silently continuing on unknown-state data is the failure mode ADR-0008's
  recovery contract exists to prevent.
- **Restart-on-gap** (discard all state and wait for the next full snapshot
  unconditionally) — rejected: correct but wasteful when only a few deltas
  were missed; buffering the deltas that arrive during recovery and replaying
  the ones still valid after rebasing recovers faster without sacrificing
  correctness.
- **Treating staleness as a risk decision** — rejected: `cpp-participant-risk`
  does not exist before M5, and conflating "this data is untrustworthy" with
  "here is what to do about a position" would require inventing risk policy
  early, which the plan explicitly forbids.
- **Two separate reconstruction types, one MBO and one MBP** — rejected: the
  frozen acceptance criteria describe one book that behaves differently
  depending on what the feed supplies, not two books; a single type with an
  optional order-level layer says that honestly.

## Consequences

- A future M5 risk consumer reads the same staleness signal this ADR defines
  (`BookBuilder::is_stale`) without this module changing.
- M9's paper feed reuses the same recovery path — buffer, rebase, replay —
  since nothing in it is specific to the M2 replay fixture format.
- Order-level and price-level reconstruction share one test suite
  (`test_book_builder.cpp`) rather than two independently-maintained ones.

## Verification

- `tests/cpp/unit/test_sequence_tracker.cpp` — each of the four diagnostics in
  isolation, and a mixed sequence exercising all four in order.
- `tests/cpp/unit/test_book_builder.cpp` — full-depth snapshot reconstructs
  expected levels and quantities (AEGIS-064); an ordered incremental sequence
  matches a golden book (AEGIS-065); order-lifecycle fixtures (add, modify,
  remove) reconstruct the correct order-level view (AEGIS-066); an
  aggregated-only delta sequence with no order identity reconstructs the
  correct price levels (AEGIS-067); `common::ManualClock`-driven staleness by
  elapsed time and by consecutive-gap count (AEGIS-069); a detected-gap →
  buffer → snapshot → replay sequence ends in the same state an uninterrupted
  feed would have reached (AEGIS-070).
- `tests/cpp/unit/test_stale_data_response.cpp` — a `kDelayed`-annotated M2
  fault stream drives the book stale and its top-of-book output is withheld
  while stale (AEGIS-060).
- `tests/cpp/unit/test_feed_recovery.cpp` — `kMissing`, `kDuplicated` and
  `kSequenceGap` M2 fault streams each drive a detectable diagnostic and a
  successful recovery to correct final state (AEGIS-061).

## Owner approval

Authorized under the owner-approved M3 plan of record
(`experiments/plans/M3.md`), 2026-08-12.
