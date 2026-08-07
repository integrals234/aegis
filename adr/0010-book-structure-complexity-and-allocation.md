# ADR-0010: Book structure, complexity claims and per-instance allocation

- Status: Accepted
- Date: 2026-08-07
- Requirement IDs: AEGIS-036, AEGIS-037, AEGIS-038, AEGIS-041
- Milestone: M1

## Context

M1 needs `OrderBook` to give an `O(1)`-flavored lookup/cancel by `OrderId`
(AEGIS-036), a FIFO queue per price level that does not allocate once warmed
(AEGIS-037), a documented price-level container with a stated complexity and
a finite price domain (AEGIS-038), and — later, when the invariant checker
lands — two invariant scopes wide enough to check mid-match state without
being so wide they fire on a legitimately crossed book. It also needs a way
to *measure* "no allocations", not just assert it in a comment, without
introducing the process-global `operator new` override
`configs/architecture_rules.yaml`'s `mutable_globals_forbidden_in` would
reject anyway (a global counter is exactly the hidden global state that
defeats deterministic replay across concurrent test runs).

## Decision

### Index-handle slab, never pointers

`cpp/exchange/order_book/order_storage.hpp`'s `OrderStorage` holds
`OrderNode`s in a `std::pmr::vector`, addressed by index. A free list of
**indices** — never pointers or references — tracks reusable slots, so a
slab growth event (which can relocate every element) cannot dangle a handle
held elsewhere. This is deliberately *not* the M8 pool: no generation
counters, no cross-book sharing, no zero-allocation *latency* claim
(AEGIS-042/043 own those; this ADR's claim is about allocation *counts*,
never about time).

### `OrderId` lookup: expected O(1), not claimed O(1)

`OrderBook::find`/`cancel` go through a pre-sized `std::pmr::unordered_map`
from `OrderId` to slab index — one hash lookup plus one O(1) intrusive
queue unlink, no price-queue scan (AEGIS-030, AEGIS-036). Documentation says
*expected* constant-time lookup over a pre-sized index, worst case linear in
bucket occupancy under adversarial hashing; `configs/claims_policy.yaml`'s ban
on claiming every book operation runs in strictly constant time is exactly
why the wording stops short of that claim.

### Price-level index: `MapLevelIndex`, expected O(log D)

`LevelIndex` (`cpp/exchange/order_book/level_index.hpp`) is an interface so
a later milestone can substitute a tick array or bitset without touching
matching (AEGIS-042/043, M8) — `configs/claims_policy.yaml`'s ban on strict
`O(1)` claims is exactly why this seam exists rather than a single hardcoded
container. `MapLevelIndex` (`std::pmr::map`, one comparator instantiated
twice — descending for bids, ascending for asks, rather than a near-duplicate
pair of classes) is the M1 baseline: expected O(log D) insert/find/erase,
where D is `InstrumentSpec::price_domain_size()` — the finite, documented
price-domain cardinality `(ceiling − floor)/tick + 1` AEGIS-038 requires.
`next_price_after` (an `upper_bound` under the same comparator) is what lets
`FifoPolicy` walk a multi-level sweep one level at a time instead of
materializing the whole side, which is what keeps matching output-sensitive
(AEGIS-039, ADR-0009).

### Injected `memory_resource`, not a global override

No layer under `cpp/exchange` overrides `operator new`. `OrderStorage`, both
`MapLevelIndex` instances, `order_index_` and `live_client_ids_` all accept a
`std::pmr::memory_resource*` at construction, defaulting to
`std::pmr::get_default_resource()` so every pre-existing call site is
unaffected. `OrderBook`'s constructor takes one resource and threads it
through all five — allocation is then attributable to the one book that owns
it, deterministic, and needs no test-ordering discipline the way a
process-global counter would.

### Measuring "zero allocations" honestly for node-based containers

`OrderStorage`'s slab is a `std::pmr::vector` with its own free list: once
`reserve()`d, it calls its resource exactly once (for the reservation) and
never again as long as live orders stay within capacity — a `reserve()`d
vector genuinely never re-allocates on `push_back`-shaped growth within
capacity. `order_index_` and `live_client_ids_`, and the level maps, are
node-based (`std::pmr::unordered_map`/`std::pmr::map`): calling `reserve()`
on an `unordered_map` sizes its *bucket array*, never its per-node storage,
so **every** insert allocates a node and **every** erase deallocates one —
that is a property of how the standard specifies these containers, true for
any resource, not a defect in this book.

`tests/cpp/property/test_allocation_counters.cpp` therefore measures the
honest claim: a `std::pmr::unsynchronized_pool_resource` sits between the
book and a counting resource
(`tests/cpp/support/counting_resource.hpp`); warmup runs the exact
steady-state add/cancel pattern once to populate the pool's per-size free
lists, the counters reset, and the same pattern runs again. The steady-state
cycle never lets a price level's order count reach zero (so
`LevelIndex::erase` is never called) and never touches a price outside the
warmed set (so no level map ever inserts a new key) — the claim is
"allocation-free once warmed for a workload that does not grow the working
set", not "allocation-free unconditionally". `CountingResource` counts calls
made *to* it and forwards everything to its own upstream; it is not a pool
itself, so the counts measure exactly what reached the counted boundary.

### Invariant scope (recorded here now; the checker lands in slice 8)

Two scopes, both eventually implemented as
`check_invariants(const OrderBook&, InvariantScope)`:

- **`kStructural`** — holds at every point the book is observable, including
  mid-match: order-index ↔ queue bijection, free list disjoint from the live
  set, every level's `aggregate_quantity` equal to the sum of `remaining`
  over its queue, `0 < remaining ≤ original_quantity`, priorities strictly
  increasing along each queue, bids descending / asks ascending in the level
  index.
- **`kQuiescent`** — the structural set plus invariants meaningful only once
  a command has fully applied: the book is uncrossed (`best_bid <
  best_ask`), no emptied level is retained in the index, and P1 per-order
  conservation (ADR-0011 §4.10) holds for every live order.

The uncrossed-book invariant is checked **only at command boundaries**,
never mid-match: an aggressor is legitimately crossed with the book for the
duration of matching, and asserting otherwise mid-loop would either be false
or force the engine into an artificial shape purely to satisfy the checker.

## Alternatives considered

- **A global `operator new` override with a process-wide counter.** Rejected:
  it is exactly the hidden global mutable state
  `configs/architecture_rules.yaml` bans in this layer, it would make
  allocation counts a property of whatever else runs in the same test
  binary, and it would need test-ordering discipline no other AEGIS test
  layer requires.
- **Claiming strict O(1) lookup.** Rejected: `configs/claims_policy.yaml`
  forbids it outright, and it is not true under adversarial hashing or
  worst-case bucket occupancy regardless.
- **Testing "zero allocations" against the raw counted resource with no
  pool.** Rejected: it would fail for `order_index_`/`live_client_ids_`
  regardless of how correctly `OrderBook` is implemented, because
  node-based containers allocate per element by specification — the test
  would be measuring a property of `std::unordered_map`, not of this book.

## Consequences

- Any future container swapped into `OrderBook` (e.g. AEGIS-042/043's M8
  work) that is node-based must be evaluated against a pool the same way, or
  the zero-allocation test must be revisited — this ADR is the place that
  revisit gets recorded.
- `docs/EXCHANGE_CORE.md` states the complexity wording verbatim
  ("expected O(1) lookup over a pre-sized index, worst case linear in
  bucket occupancy"; "expected O(log D) level insert/find/erase") so it
  cannot drift from what this ADR says without the doc changing too.
- Benchmarks (AEGIS-036, AEGIS-039, slice 11) measure wall-clock time, which
  is explicitly a *different* claim from this ADR's allocation-count claim;
  `docs/BENCHMARK_POLICY.md`'s `local_non_comparable: true` labelling
  applies to those, not to this one.

## Verification

- `tests/cpp/property/test_allocation_counters.cpp` — the steady-state
  zero-allocation claim (AEGIS-037).
- `tests/cpp/property/test_order_index.cpp` — lookup/cancel correctness
  under insert/erase churn (AEGIS-036).
- `tests/cpp/unit/test_level_index.cpp`,
  `tests/cpp/property/test_match_visits.cpp` — level-index behavior and
  output-sensitive matching bound (AEGIS-038, AEGIS-039).
- `tests/cpp/property/test_book_invariants_fuzz.cpp` (slice 8) — both
  invariant scopes under generated command streams.

## Owner approval

Confirmed by the owner per experiments/plans/M1.md §11 (general plan
approval; no scope-specific owner decision named for this ADR beyond the
plan's own corrections 1 and 5).
