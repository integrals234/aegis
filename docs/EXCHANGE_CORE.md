# Exchange Core (M1)

The deterministic limit-order-book, sequencing and matching core built in M1.
This document states the concrete facts a caller of `cpp/exchange/**` needs —
identifier spaces, the price/quantity grid, documented complexity, and the
two invariant scopes — that the ADRs argue for and this document just states.
Full reasoning lives in `adr/0009` through `adr/0013`; `docs/LIMITATIONS.md`
records what this core deliberately does not do.

## Identifier spaces

Three distinct strong types, none convertible to another, each with its own
counter (ADR-0012, `adr/0013-exchange-snapshot-and-recovery.md`):

| | `CommandSequence` | `EventSequence` | `OrderId` |
|---|---|---|---|
| Assigned by | `Sequencer`, on every command received (including one that will be rejected) | `EventLog`, on every canonical event emitted | `MatchingEngine`, on acceptance only |
| Starts at | 1 | 1 | 1 |
| Cardinality | one per command | one or many per command (accept + trades + terminations) | one per accepted order; a cancel-replace allocates a **new** one |
| Used as | FIFO priority key (`Priority::from(CommandSequence)`) and the causal reference on every event | the ordering key of the canonical output stream | the identity cancel/modify target and trades reference |
| Serialized as | u64 LE, journal/`causing_command_sequence` | u64 LE, `Envelope.sequence` | u64 LE, event payloads and snapshot order records |
| Restored from | snapshot header `next_command_sequence` | snapshot header `next_event_sequence` | snapshot header `next_order_id` |

`Priority` is constructed only via `Priority::from(CommandSequence)` — never
from insertion order, arrival time, or anything else. Two commands sharing an
`EventTime` still order deterministically, because the sequencer, not the
clock, breaks the tie (AEGIS-027).

## Price and quantity grid

`PriceUnits`/`QuantityUnits` are strong `int64` types — the canonical storage
and wire unit. `InstrumentSpec` declares the grid on top of them:

```
PriceUnits    price_floor_units;      // grid origin, inclusive lower bound
PriceUnits    price_ceiling_units;    // inclusive upper bound
int64_t       tick_size_units;        // > 1 in every fixture
QuantityUnits min_quantity_units;
QuantityUnits max_quantity_units;
int64_t       lot_size_units;         // > 1 in every fixture
```

Validation order (ADR-0009 §4.4): price out-of-band, then off-tick; quantity
non-positive, then off-lot, then out-of-range
(`cpp/exchange/order_book/instrument.hpp`).

**Price-domain cardinality** is finite and documented:
`(ceiling - floor) / tick + 1` (`InstrumentSpec::price_domain_size()`) —
the assumption AEGIS-038 requires stated, not left implicit.

**Lot-alignment is closed under matching (P4, ADR-0013's Consequences
section; asserted by `tests/cpp/property/test_quantity_conservation.cpp`
over generated sequences).** Every accepted quantity is a multiple of
`lot_size_units`; a fill is `min(remaining_maker, remaining_taker)`; the
minimum of two multiples of L is itself a multiple of L. So every fill,
residual and level aggregate stays a lot multiple by construction, not by a
runtime check on each one.

M1 attaches no currency, multiplier, exponent or display scale to a price
unit (AEGIS-011, M2), and computes no notional — there is no `price ×
quantity` product to overflow.

## Documented complexity

`configs/claims_policy.yaml` bans claiming every book operation runs in
strictly constant time; the wording below is deliberately short of that
claim (ADR-0010):

- **`OrderId` lookup/cancel**: expected O(1) via a pre-sized
  `std::pmr::unordered_map` from `OrderId` to slab index, worst case linear
  in bucket occupancy under adversarial hashing. One hash lookup plus one
  O(1) intrusive-queue unlink — no price-queue scan (AEGIS-030, AEGIS-036).
- **Price-level operations**: expected O(log D), D = price-domain
  cardinality above, via `MapLevelIndex` (`std::map`) behind the
  `LevelIndex` interface — a later milestone can substitute a tick array or
  bitset without touching matching (AEGIS-042/043, M8).
- **Matching a taker**: O(k) in the number of resting orders actually
  consumed, not O(book size) — `MatchingPolicy::match` walks only the levels
  and orders a fill touches (AEGIS-039). `tests/cpp/property/test_match_visits.cpp`
  asserts visited count tracks consumed count;
  `experiments/evidence/AEGIS-039/multi_fill_k*.json` shows allocation and
  latency scaling with `k` across `k ∈ {2, 4, 8, 16, 32}`.

## Invariant scopes (AEGIS-041, ADR-0010)

`check_invariants(const OrderBook&, InvariantScope)`
(`cpp/exchange/order_book/invariants.hpp`) has two scopes:

- **`kStructural`** — holds at every point the book is observable, including
  mid-match: the order-index/queue bijection; the free list disjoint from
  the live set; every level's `aggregate_quantity` equal to the sum of
  `remaining` over its queue (P3); `0 < remaining <= original_quantity`;
  priorities strictly increasing along each queue; bids descending / asks
  ascending in the level index.
- **`kQuiescent`** — `kStructural` plus invariants meaningful only once a
  command has fully applied: the book is uncrossed (`best_bid < best_ask`),
  no emptied level is retained in the index, and P1 per-order conservation
  holds for every live order (§4.10 of `experiments/plans/M1.md`; ADR-0011).

The uncrossed-book check is `kQuiescent`-only and checked only at command
boundaries, never mid-match: an aggressor is legitimately crossed with the
book for the duration of matching, and asserting otherwise would either be
false or force the engine into a shape that hides the transition.
`tests/cpp/property/test_book_invariants_fuzz.cpp` calls `kQuiescent` after
every generated command; `tests/cpp/unit/test_book_invariants.cpp` proves
both scopes actually fire, against hand-corrupted fixtures built by bypassing
`MatchingEngine`.

## Conservation properties (§4.10, ADR-0013's Consequences section)

- **P1 — per-order.** `original_quantity(o) = cumulative_filled(o) +
  remaining(o) + cancelled_quantity(o)` for every accepted order at every
  quiescent point. `cancelled_quantity` accumulates: an explicit cancel, an
  accepted market order's unfilled residual (`kResidualCanceled`), a
  priority-retaining quantity decrease's delta, and a cancel-replaced
  order's whole remaining quantity.
- **P2 — aggregate, both sides.** `Σ buy cumulative_filled == Σ trade
  quantity == Σ sell cumulative_filled`; `Σ all cumulative_filled == 2 × Σ
  trade quantity` (one maker + one taker credited per fill).
- **P3 — book aggregate.** `Σ level aggregate_quantity == Σ live resting
  remaining(o)`, checked globally and per level.
- **P4 — lot alignment.** Every `remaining`, `cumulative_filled`,
  `cancelled_quantity` and level aggregate is a multiple of
  `lot_size_units`.

`tests/cpp/property/test_quantity_conservation.cpp` asserts all four over a
generated operation sequence, with the ledger built from the **decoded event
stream**, never from book internals — a bug that corrupted both the book and
its own accounting the same way would otherwise go uncaught.

## Order lifecycle (ADR-0011)

- **Market orders never reject for lack of liquidity.** Accepted; a partial
  or empty-book match terminates the unfilled residual as
  `kResidualCanceled`, never rests. A market order carrying a price is
  `kPriceOnMarketOrder` — a validation failure decided before the order
  exists, not a liquidity outcome.
- **Modify.** `new_quantity_units` is the order's new *total* (FIX `OrderQty`
  convention). A price-unchanged decrease is in place and retains priority.
  A price change or a quantity increase is cancel-replace: the original
  terminates (`kReplaced`) and a new `OrderId` with a new `Priority` rests
  at the tail. `new_quantity_units` below `cumulative_filled` is
  `kModifyBelowFilled`.
- **Client order IDs.** Scoped to `(participant_id, client_order_id)`, live
  orders only. Two participants may reuse the same value; one participant
  may not while its order is live. Reuse after termination is accepted —
  there is no `kOrderAlreadyTerminal`, because retaining one would require
  an unbounded tombstone set M1 must not invent. Cancel/modify of an
  unknown, already-terminal, or never-existent `OrderId` is uniformly
  `kUnknownOrderId`.

## Snapshot and restore (ADR-0013)

`ExchangeSnapshot` (`cpp/exchange/state/snapshot.hpp`) carries the three
counters plus every resting order across every registered instrument, in
canonical `(instrument_id, price_units, priority)` order — the order that
lets `restore_orders_into` replay them through `OrderBook::add` and
reconstruct each level's FIFO queue exactly. `read_snapshot` refuses an
unknown `snapshot_version` or a header counter that does not dominate its
own contents (`kCounterInconsistent`). Acceptance is **continuation
equality**, not round trip alone: a run split as *first half → snapshot →
restore into a fresh `ExchangeNode` → second half* emits byte-identical
canonical output, including every `EventSequence` and `OrderId` assigned
after the split, to the same commands run uninterrupted
(`tests/cpp/unit/test_snapshot_roundtrip.cpp`,
`tests/replay/test_exchange_determinism.py`).

## What is explicitly not here

No self-trade prevention, no time-in-force beyond immediate matching, no
auctions, no pro-rata allocation, no multi-threaded matching (one writer per
book, AEGIS-047 is M8), no market-data publication (`cpp/exchange/market_data`
stays empty until M3, ADR-0012). See `docs/LIMITATIONS.md`.
