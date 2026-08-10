# ADR-0019: Deterministic fault injection

- Status: Accepted
- Date: 2026-08-10
- Requirement IDs: AEGIS-060, AEGIS-061
- Milestone: M2

## Context

M2's replay core (slice 9) and pacing (slice 10) both preserve the
canonical event sequence exactly. Fault injection is the first place M2
deliberately perturbs it -- delayed, missing, duplicated and sequence-gap
events (AEGIS-060/061; AEGIS-062/063's market/execution stress kinds are
this same mechanism, extended in slice 12). The risk worth designing
against explicitly: it would be easy to build something that "injects
faults" by reaching for a seeded random-number generator, which looks
deterministic (same seed, same sequence) but actually couples the fault
positions to the RNG's own implementation -- a compiler upgrade or standard-
library change could silently relocate every fault. And it would be
equally easy to let a fault's own bookkeeping mutate a `ReplayEvent`'s
canonical-order fields, quietly reopening the ordering guarantee three
prior slices exist to close.

## Decision

**Faults are explicit, committed data -- never sampled.** `FaultRule` names
an exact `record_index` and kind; there is no random selection anywhere in
`DeterministicFaultInjector`. "Deterministic" here means what it means
everywhere else in this milestone: the same rule set applied to the same
stream produces the identical result on every run, on every machine,
forever -- not merely "reproducible given the same seed on the same RNG
implementation."

**No fault mutates a canonical-order field.** `event_time`,
`source_sequence`, `contract_symbol` and `record_index` are read, copied
into the output, and never rewritten by any fault kind -- including
`kSequenceGap`, which is deliberately an **annotation** ("a gap of this
magnitude occurred in the source's own sequence numbering here") rather
than an actual rewrite of the record's `source_sequence`. Mutating
`source_sequence` was considered and rejected: it is the second component
of the canonical order (`replay_event.hpp`, slice 1), so perturbing it
would risk moving a record's *position*, not just flagging a fact about
it, for a milestone that spent three slices establishing that position is
exactly one legal thing. The annotation says "a consumer should notice a
gap here"; it does not manufacture one in the ordering itself.

**One mechanism, a growing enum -- not one class per fault kind.**
`FaultKind` is a single enum `DeterministicFaultInjector::apply` switches
over; slice 12 adds more values to the same enum rather than introducing a
second injector type. This mirrors the roll-policy family's own choice
(M2 slice 6) in the opposite direction: roll policies are one-class-per-
policy because each has genuinely different selection *logic*; fault kinds
share identical injection *logic* (look up a rule by `record_index`,
annotate or drop or duplicate) and differ only in which of three outcomes
applies, which a switch expresses more directly than four nearly-identical
classes would.

**Nothing is silently lost.** A `kMissing`-faulted record is genuinely
absent from `FaultInjectionResult::events` (that is the fault), but it is
never simply gone -- `dropped` records its `record_index` and the reason.
An ambiguous rule set (two rules targeting the same `record_index`) is
rejected outright rather than resolved by picking one arbitrarily: a
caller that meant to compose two faults on one record has a bug, and
`apply` refuses to guess which fault they meant.

**Mechanism only -- no response simulated.** `AEGIS-060`'s "stale-data
response" and `AEGIS-061`'s "recovery" both need a participant-side
consumer that watches the annotated stream and reacts; that consumer does
not exist before M3 (`configs/architecture_rules.yaml` dates the
participant pipeline M3). Building a response here would be implementing
M3 early, which the owner's slice 8-13 instructions explicitly forbid.
What M2 owns and delivers is the fact that a fault occurred, attached to
the exact record it affects, deterministically.

## Alternatives considered

- **A seeded PRNG selecting which records to fault** -- rejected; see
  Decision. A fixed rule list is not less general: a caller who wants
  "randomly" distributed faults can generate the rule list themselves,
  once, and commit it, which is exactly what every other deterministic
  fixture in this milestone already does (e.g. the seeded quality-
  corruption suite, M2 slice 5).
- **Rewriting `source_sequence` for `kSequenceGap`** -- rejected; see
  Decision.
- **One derived class per fault kind (mirroring `RollPolicy`)** --
  rejected; see Decision. Revisited if a future fault kind ever needs
  genuinely different injection logic rather than a different outcome of
  the same lookup-and-branch.
- **Silently dropping a `kMissing` record with no accounting** -- rejected:
  it would make "how many faults were actually injected" unanswerable from
  the result alone, defeating the evidence this mechanism exists to
  produce.
- **Resolving an ambiguous rule set by applying the first rule found** --
  rejected: picking one silently would hide a caller's mistake instead of
  surfacing it.

## Consequences

- Slice 12 (market/execution stress) touches only `FaultKind`'s enumerator
  list and the `switch` in `DeterministicFaultInjector::apply` -- no new
  type, no new public function signature.
- A future M3 stale-data-response consumer and a future M5 risk-response
  consumer both read the same `FaultInjectionResult` shape this slice
  defines; neither needs this module to change to be built.
- Because no fault ever perturbs `record_index`, a fault-injected stream's
  `dropped` list plus its `events` list can always be cross-referenced
  against the original input's full `record_index` range to prove nothing
  vanished unaccounted-for -- a property a future roll/replay audit report
  can rely on without re-deriving it.

## Verification

- `tests/cpp/unit/test_fault_injection.cpp` -- no rules leaves the stream
  untouched and unannotated; each of the four kinds in isolation (delayed
  stays present and annotated, missing is removed and accounted for,
  duplicated appears twice consecutively with only the copy annotated,
  sequence-gap annotates without mutating `source_sequence`); multiple
  independent rules on one stream; an ambiguous rule set throws; and the
  whole mechanism is deterministic across repeated calls with the same
  input.

## Owner approval

Authorized as part of M2 slice 11 under the owner-approved M2 plan of
record (`experiments/plans/M2.md`, rev. 4) and the owner's slice 8-13
build-first continuous-execution prompt, 2026-08-10.
