# ADR-0018: Replay core and determinism

- Status: Accepted
- Date: 2026-08-10
- Requirement IDs: AEGIS-058, AEGIS-054, AEGIS-055, AEGIS-056, AEGIS-057
- Milestone: M2

## Context

M2 slice 1 built `ReplayEvent` and its total canonical order
(`cpp/replay/replay_event.{hpp,cpp}`) but deliberately nothing else --
"no stream reader, no virtual clock, no pacing, no fault injection. Those
are slices 9 through 12." This slice is slice 9: the minimum machinery that
turns a validated, canonically-ordered sequence of `ReplayEvent`s into a
deterministic replay -- loadable from disk, driven through a clock that
never touches the wall clock, with a reproducibility digest and a
cursor/resume mechanism. `cpp-replay` must not depend on any
`cpp-exchange-*` layer (`configs/architecture_rules.yaml`, and
`cpp/replay/CMakeLists.txt`'s own comment since slice 1); everything here
is exchange-independent by construction, not by convention.

## Decision

**One place reads and validates input; a separate class drives it.**
`load_replay_stream` (`replay_stream.hpp/.cpp`) is the only code that opens
a file. It parses JSON-Lines records carrying the same four canonical
fields `ReplayEvent` already names (`event_time_ns`, `source_sequence`,
`contract_symbol`, `record_index` -- the same field names
`python/futures/schema.py`'s `futures_bar.v1` uses for the equivalent
concepts, so no name-translation layer exists), and validates the loaded
sequence is strictly increasing under `canonical_less` (slice 1). Two
canonically-equal records is `kDuplicateKey` (a `record_index` collision --
should be impossible from correctly-assigned input, and is treated as an
input defect, never silently deduplicated); anything not `canonical_less`
of its predecessor is `kOutOfOrder`. **`record_index` is read, never
recomputed** -- this module has no code path that could reassign it, per
the M2 plan of record's explicit instruction.

Failure is returned, not thrown: `ReplayStreamResult` mirrors
`events::DecodeResult`/`exchange::SnapshotReadResult`'s existing hand-rolled
result-type pattern (documented there as the deliberate stand-in for
`std::expected` under this project's C++20 target, ADR-0005) rather than
introducing a third, differently-shaped error convention.

**The virtual clock's determinism is structural, not a mode flag.**
`VirtualClock` (`virtual_clock.hpp/.cpp`) implements `common::WallClock`
(`cpp/common/clock.hpp`, same interface `ManualClock` already implements
for tests) and has exactly one mutator, `advance_to(EventTime)`; there is
no code path in this class that reads `std::chrono::system_clock` or
sleeps. "Deterministic benchmark mode... without wall-clock sleeps" is
therefore not a behavior to configure -- it is the only behavior this clock
has. `advance_to` refuses to move backward, matching the invariant
`load_replay_stream` already establishes upstream (a validated stream never
asks the clock to go backward); a caller that tries anyway has a bug at the
call site, not a legitimate replay scenario.

**The manifest digest is FNV-1a 64-bit, not a cryptographic hash.**
`compute_manifest` (`replay_manifest.hpp/.cpp`) exists to make "repeated
runs produce identical outputs" cheaply checkable -- compare a 64-bit
number and a handful of scalar fields instead of diffing a potentially
large emitted stream. That is a reproducibility check, not an integrity or
tamper-resistance claim, so FNV-1a was chosen over a cryptographic
alternative deliberately: it needs no new dependency (`nlohmann_json`,
already used for parsing, and standard-library-only hashing), and its only
job is "did the content or order change", which a non-cryptographic
digest answers exactly as well.

**The engine owns cursor/resume; loading owns nothing about position.**
`ReplayEngine` (`replay_engine.hpp/.cpp`) takes an already-validated vector
and a `VirtualClock&`; `cursor()` reports the `record_index` of the last
emitted record, and `resume_from(RecordIndex)` seeks to the position
immediately after it. Resuming is a linear scan for the matching
`record_index` -- correctness over performance is this milestone's
ordering (CLAUDE.md's quality order places performance after correctness,
determinism and reproducibility; that tradeoff is M8's, not this slice's).

## Alternatives considered

- **A cryptographic digest (e.g. SHA-256) for the manifest** -- rejected:
  the manifest's job is reproducibility detection, not integrity or
  tamper-resistance, and a cryptographic hash would add a dependency this
  milestone does not otherwise need for no additional guarantee this slice
  actually uses.
- **Throwing exceptions from `load_replay_stream` instead of a result
  type** -- rejected in favor of matching the two existing precedents
  (`DecodeResult`, `SnapshotReadResult`) for "validate untrusted structured
  input"; a third, differently-shaped convention for the same kind of
  operation would be needless inconsistency.
- **Letting the virtual clock silently clamp a backward request to
  no-op** -- rejected: a caller asking to go backward is exactly the kind
  of bug deterministic replay exists to surface, and silently absorbing it
  would hide that the upstream validation was bypassed somehow.
- **Storing `position_` as a `RecordIndex` directly instead of a vector
  index** -- rejected: `record_index` is a property of a specific record,
  not guaranteed to equal its position in every conceivable canonical
  ordering (it is only the final tie-break, not the primary sort key), so
  conflating "how many records have been emitted" with "the last emitted
  record's own index" would be a category error even though the two
  happen to coincide whenever a stream sorts records index-ascending.

## Consequences

- Slice 10 (pacing) adds `next_with_pacing` to `ReplayEngine` without
  touching `load_replay_stream`, `VirtualClock` or the manifest -- pacing
  is purely about *when* a caller might choose to act on an already-
  determined event sequence, never about *which* events or *what order*.
- Slices 11-12 (fault injection) operate on the same validated
  `ReplayEvent` vectors this slice produces; neither slice needs to touch
  the loader or the clock.
- Slice 13's bindings expose `canonical_less`/`canonical_compare` (already
  public since slice 1) to Python; nothing in this slice needed to change
  for that to be possible, confirming the "no edge to cpp-exchange, minimal
  public surface" design holds up under the binding requirement too.

## Verification

- `tests/cpp/unit/test_replay_stream.cpp` -- loads a valid fixture,
  rejects a missing file, a malformed record, an out-of-order record and a
  duplicate canonical key, each with the specific error kind asserted.
- `tests/cpp/unit/test_virtual_clock.cpp` -- advances forward correctly,
  refuses to advance backward, and never reads the system clock (asserted
  by never calling anything but `advance_to`/`now_utc` across the whole
  test file).
- `tests/cpp/unit/test_replay_manifest.cpp` -- digest is stable across two
  independent computations over the same input, changes when content or
  order changes, and `fnv1a64` matches an independently written reference
  computation for a known input/output pair.
- `tests/cpp/unit/test_replay_engine.cpp` -- `next()` drains a stream in
  canonical order while advancing the clock to each event's own time;
  `next_group()` groups same-timestamp records as one step; `cursor()`/
  `resume_from()` reproduce the tail of an uninterrupted run exactly, the
  cursor/resume property `experiments/plans/M2.md` section 7 names.

## Slice 10 addendum: pacing modes (AEGIS-054..057)

### Context

Slice 9 built the engine that drains a validated stream through a virtual
clock with no automatic timing at all. The four approved pacing modes
(AEGIS-054 original speed, AEGIS-055 accelerated, AEGIS-056 fixed rate,
AEGIS-057 step) all describe *when* a caller might act on an event, never
*which* events or in *what order* -- so the natural design question is
where that "when" computation lives without letting it leak into what the
engine already guarantees deterministic.

### Decision

**Pacing computes a virtual wait; it never sleeps, and the engine never
sleeps on its behalf.** `PacingPolicy::wait_before(previous, current)`
returns a `common::Duration` -- a pure computation over two already-fixed
events. `ReplayEngine::next_with_pacing` pairs the next emitted event with
that computed wait and returns both to the caller, who decides (in a
future CLI, not built in M2) whether to act on it. This is what keeps
"deterministic test paths free from real sleeping or scheduler dependence"
true by construction: there is no code path in this module that could
introduce a real delay, so a test asserting on wait values never becomes
flaky no matter how it's run.

**The first emission from an engine instance always has zero wait,
regardless of policy.** There is no predecessor to compute a gap from, and
inventing one (e.g. treating the stream's own start as a synthetic
"previous" event) would be a guess this ADR declines to make. A caller
that wants a specific pre-roll delay before the first event states it
explicitly, outside this mechanism.

**Four concrete policies, one shared interface, no fifth "auto-detect"
mode.** `OriginalSpeedPacing` returns the real gap; `AcceleratedPacing`
divides it by a caller-supplied multiplier (constructor-validated strictly
positive -- zero or negative has no "speed" meaning); `FixedRatePacing`
ignores the gap entirely and returns a constant configured interval
(validated non-negative); `StepPacing` always returns zero, because
AEGIS-057's "one event or one timestamp group at a time" is already fully
satisfied by `ReplayEngine::next`/`next_group` (slice 9) doing the
advancing -- `StepPacing` exists so the pacing interface has a uniform
fourth member, not because it computes anything itself.

**Pacing cannot change the event sequence -- proven, not assumed.** Since
`wait_before` never touches `events_`, `position_` or the clock's
advancement logic (that stays in `next()`), no pacing policy can reorder,
duplicate or drop a record; `next_with_pacing` still calls the same
`next()` internally. The test suite asserts this property directly (all
four modes run over the same input produce the identical `record_index`
sequence) rather than relying on the implementation argument alone.

### Alternatives considered

- **Having the engine actually sleep for the computed duration** --
  rejected: it would make every pacing test's runtime depend on wall-clock
  behavior, exactly what "free from real sleeping or scheduler dependence"
  forbids, for no benefit M2 needs (no CLI consumes the wait yet).
- **Synthesizing a "virtual predecessor" for the first event** -- rejected
  as an invented fact; see Decision.
- **A single parameterized pacing class with a mode enum instead of four
  policy classes** -- rejected: the existing `PacingPolicy`/`RollPolicy`
  (M2 slice 6) pattern of one small class per behavior keeps each mode's
  validation (multiplier > 0, interval >= 0) next to the mode it belongs
  to, rather than in a shared branch that has to remember which validation
  applies to which enum value.

### Consequences

- Slices 11-12 (fault injection) do not touch `pacing.hpp/.cpp` at all --
  fault injection and pacing are orthogonal concerns over the same
  validated stream, confirmed by neither slice needing to import the
  other's header.
- A future CLI (M2 slice 14 or later) that wants to actually pace replay
  in real time wraps `next_with_pacing`'s returned duration in its own
  sleep call; that wrapper is explicitly out of scope for this ADR and
  this milestone.

### Verification

- `tests/cpp/unit/test_pacing.cpp` -- each policy's `wait_before` formula
  in isolation (including constructor validation for `AcceleratedPacing`/
  `FixedRatePacing`), the first-emission-is-always-zero rule, `next_with_pacing`
  exhaustion, and the shared invariant that all four modes emit the
  identical canonical `record_index` sequence for the same input.

## Slice 13 addendum: the unified feed boundary (AEGIS-059)

### Context

AEGIS-059 asks for a feed abstraction and a historical implementation.
The C++ binding side of slice 13 (exposing `canonical_less` to Python as
`sort_canonical`) is documented in ADR-0005's own slice 13 addendum, not
repeated here. What this addendum covers is the Python-side consumer of
canonical order: `python/futures/replay.py`'s `Feed` protocol and
`HistoricalReplayFeed`, which take already-ingested `futures_bar.v1`
records (`python/futures/ingest.py`, file order, not replay order) and
impose the actual replay order on them.

### Decision

**`HistoricalReplayFeed`'s `cursor()`/`resume_from()` mirror
`ReplayEngine::cursor()`/`resume_from()` (this ADR's main Decision) in
Python terms, deliberately.** `cursor()` reports the `record_index` of
the most recently emitted record (`None` before the first); `resume_from`
seeks to the position immediately after a given `record_index`, never
re-emitting it or anything before it -- the identical contract, so a
future consumer that understands the C++ engine's resume semantics
already understands the Python feed's.

**The canonical sort is native Python (`sort_canonical`,
`python/futures/replay.py`), not a call through the compiled bindings.**
A feed that only works when `aegis_bindings` is built would make the
whole `python/futures` layer conditionally functional on a C++ toolchain
being present, which no other module in this layer requires. The
bindings' own `sort_canonical` (ADR-0005 addendum) exists to *prove* the
two agree, not to be a required dependency of this one.

**Sorting happens once, at construction, not lazily per iteration.** The
feed's records are fixed the moment `HistoricalReplayFeed.__init__` runs;
nothing about iteration re-sorts or re-derives order, matching this ADR's
existing separation between "load and validate" and "drive" (Decision,
above) in Python terms.

### Alternatives considered

- **Routing the Python feed's sort through the compiled binding** --
  rejected: see Decision. Revisited only if a future milestone finds the
  native Python sort is a genuine performance bottleneck, which CLAUDE.md's
  quality order (performance after correctness, determinism, reproducibility)
  does not justify addressing now.
- **Giving `HistoricalReplayFeed` its own record_index-assignment logic**
  -- rejected: `record_index` is ingestion's responsibility
  (`python/futures/ingest.py`, unchanged since slice 4); this feed reads
  it, exactly as the C++ engine does.

### Consequences

- A future M9 live/paper feed implements the same `Feed` protocol; nothing
  about `HistoricalReplayFeed`'s internals needs to change for that to be
  possible, since the protocol only requires `__iter__`.
- A future M4 strategy or M3 participant pipeline consumes
  `HistoricalReplayFeed` (or a live feed built against the same protocol)
  without needing to know whether the records came from disk or a
  compiled engine underneath.

### Verification

- `tests/unit/test_historical_replay_feed.py` -- canonical ordering from
  scrambled input, protocol conformance, exhaustion, `cursor()`/
  `resume_from()` semantics (including the unknown-`record_index` and
  resume-at-last-record edge cases), and the empty-feed case.
- `tests/integration/test_bindings_roundtrip.py` -- the Python/C++
  `sort_canonical` symmetry tests (see ADR-0005's slice 13 addendum).

## Owner approval

Authorized as part of M2 slice 9 (replay core, AEGIS-058), M2 slice 10
(pacing modes addendum, AEGIS-054..057) and M2 slice 13 (this addendum:
unified feed boundary, AEGIS-059), all under the owner-approved M2 plan of
record (`experiments/plans/M2.md`, rev. 4) and the owner's slice 8-13
build-first continuous-execution prompt, 2026-08-10.
