# ADR-0012: Exchange sequencing, layering and determinism

- Status: Accepted
- Date: 2026-08-07
- Requirement IDs: AEGIS-005, AEGIS-027, AEGIS-036, AEGIS-038
- Milestone: M1

## Context

M1 needs an ordering authority that is independent of every wall clock, a
build graph in which no single layer can compose sequencer + book + matching +
persistence (so nothing outside the exchange app can reach all of them at
once), and a discharge path for AEGIS-005's obligation, registered at M0
closure as due at M1 (`docs/DEFERRED_VERIFICATION.md`).

`configs/architecture_rules.yaml` at M0 close declared five exchange-side
layers, one of them (`cpp-exchange-market-data`) dated M1 despite no M1
requirement asking the exchange to publish a market-data feed, and no layer
for the event log, journal or snapshot codec — code that is neither matching
nor market data. `tools/check_architecture.py`'s layer-population check
(`check_layer_population`) fails in both directions: before a layer's declared
milestone it must be empty, from it it must not be. Putting persistence code
under `market_data` to satisfy that check would be exactly the vacuous pass
the checker exists to prevent.

## Decision

**Three identifier spaces, never derived from one another:**

| | `CommandSequence` | `EventSequence` | `OrderId` |
|---|---|---|---|
| Assigned by | `Sequencer`, on every command received (including ones that will be rejected) | `state::EventLog`, on every canonical event emitted | `MatchingEngine`, on acceptance only |
| Cardinality | one per command | one or many per command | one per accepted order; cancel-replace allocates a new one |
| Serialized as | u64 LE, journal header and `causing_command_sequence` on every event | u64 LE, `Envelope.sequence` | u64 LE, every event payload and snapshot order record |

`Priority` is a distinct strong type constructed only via
`Priority::from(CommandSequence)`. FIFO priority is the `CommandSequence` of
the command that established an order's current queue position, so two
commands sharing an `EventTime` still order deterministically — the sequencer,
not the clock, breaks the tie. This is what makes AEGIS-027's price-time
priority independent of wall-clock resolution.

**Two new layers**, added to `configs/architecture_rules.yaml`:

- `cpp-exchange-state` (`cpp/exchange/state`) — journal, event log, snapshot
  codec. May depend on `cpp-common, cpp-events, cpp-exchange-sequencer,
  cpp-exchange-order-book`. This is exchange *state*, accurately named, not a
  vacuous fit inside matching or market data.
- `cpp-exchange-app` (`cpp/exchange/app`) — the composition root. May depend
  on all of the above. No other layer may legally see sequencer, book,
  matching and state simultaneously.

**`cpp-exchange-market-data` is re-dated from M1 to M3.** No M1 requirement
names an exchange-side market-data feed; the first genuine consumer of
exchange-published market data is the participant feed handler and book
reconstruction (AEGIS-064-070, module "Market Data & Book Reconstruction",
all M3). `configs/milestone_scope.yaml` independently confirms M3 is the
earliest milestone permitted to write the directory. This has direct
precedent: AEGIS-004's obligation was re-dated to M4 and AEGIS-237's to M3 at
M0 closure, in both cases because `architecture_rules.yaml` dates the layer
their acceptance depends on later than where the obligation was first
registered (`docs/DEFERRED_VERIFICATION.md`). No requirement is attached to
the `cpp-exchange-market-data` layer, so re-dating it defers no requirement —
it corrects a scope date, the same class of act.

**The engine reads no clock; `ExchangeTime` is derived.**
`ExchangeTime = max(previous_exchange_time, command.event_time)`. A
regressing input is stamped forward and counted, never rejected silently.
`MonotonicTime` appears only in the M1 benchmark driver and is never
serialized (`cpp/common/time.hpp` already refuses this at compile time via
`serialize_nanos`).

**AEGIS-005 discharge.** `python/common/determinism.py` gains
`exchange_producer(seed, root)`, which runs `aegis_exchange_replay` on a
committed scenario fixture and returns its canonical stdout.
`tools/determinism_check.py` spawns a fresh interpreter per run with a varied
`PYTHONHASHSEED` — exactly the property needed to prove the engine's output
does not depend on Python's own hash randomization leaking into anything the
engine touches. A missing replay binary raises, never skips: a skipping
determinism test is a green stub.

**Layer edges used:** `sequencer → {common, events}`; `order_book → {common,
events}`; `matching → {common, events, order_book}`; `state → {common,
events, sequencer, order_book}`; `app → all of the above`. No edge points to
`participant`, `replay`, `memory`, `queues`, `market_data` or Python.

## Alternatives considered

- **One identifier space for priority, event order and order identity.**
  Rejected: a snapshot restore that silently loses one counter reproduces
  plausible-looking output from the wrong state, and nothing would notice.
  Three snapshotted counters plus continuation equality (ADR-0013) is what
  makes that failure visible.
- **Leave `cpp-exchange-market-data` dated M1 and put event log/snapshot code
  there to pass the emptiness check.** Rejected: that is precisely the vacuous
  pass `check_layer_population` exists to prevent, and it would misname
  exchange state as market data for a requirement that does not exist yet.
- **A wall-clock `ExchangeTime`.** Rejected: it would make matching outcomes
  depend on when the process happened to run the command, which is
  irreproducible by definition.

## Consequences

- `cpp/CMakeLists.txt` gains `add_subdirectory(exchange)`, and the DAG is
  re-checked against `target_link_libraries` edges automatically by
  `tools/check_architecture.py`.
- `cpp/exchange/market_data/` stays a `.gitkeep`-only directory through M2;
  `check_layer_population` now enforces that as a checked fact, not a
  convention.
- Every later milestone that reads exchange state does so through
  `cpp-exchange-state`'s snapshot/event-log types, not by reaching into
  `cpp-exchange-order-book` or `cpp-exchange-matching` directly.

## Verification

- `tests/cpp/unit/test_exchange_types.cpp` — strong-type arithmetic, no
  implicit conversion between the three identifier spaces, `Priority::from`
  is the only construction path.
- `tests/cpp/unit/test_matching_fifo.cpp` — two commands sharing an
  `EventTime` still order deterministically by `CommandSequence`.
- `tests/unit/test_exchange_determinism_lint.py` — greps `cpp/exchange/**`
  for `system_clock`, `steady_clock`, `std::random_device`, `rand(`, `time(`.
- `python3 tools/check_architecture.py` — layer DAG and population, run every
  slice.
- AEGIS-005 discharge: `tools/determinism_check.py --producer exchange`
  (slice 10).

## Owner approval

Confirmed by the owner per experiments/plans/M1.md §11.4 (market-data
re-dating) and §11.2 (AEGIS-005 unaffected; M1 pays it).
