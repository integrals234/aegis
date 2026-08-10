# ADR-0018: Replay core and determinism

- Status: Accepted
- Date: 2026-08-10
- Requirement IDs: AEGIS-058
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

## Owner approval

Authorized as part of M2 slice 9 under the owner-approved M2 plan of
record (`experiments/plans/M2.md`, rev. 4) and the owner's slice 8-13
build-first continuous-execution prompt, 2026-08-10.
