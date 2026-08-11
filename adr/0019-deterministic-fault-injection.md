# ADR-0019: Deterministic fault injection

- Status: Accepted
- Date: 2026-08-10
- Requirement IDs: AEGIS-060, AEGIS-061, AEGIS-062, AEGIS-063
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

## Slice 12 addendum: market and execution stress (AEGIS-062, AEGIS-063)

### Context

AEGIS-062 asks for "spread widening, volatility spikes, and disappearing
liquidity"; AEGIS-063 asks for "rejection, latency, partial-fill, and
output-backpressure events." All seven sound like they need real
market-data or order-lifecycle semantics to mean anything -- a "widened
spread" implies a bid and an ask, a "partial fill" implies an order and a
quantity. `cpp-replay` has neither: `ReplayEvent` (slice 1) is identity and
ordering only, deliberately with no price/quantity payload, and no
OMS/risk layer exists before M5 to receive a fill or a rejection. The
question this addendum answers is what these seven kinds can honestly mean
at the layer M2 actually owns.

### Decision

**Same mechanism as slice 11, same `FaultKind` enum, no new type.** All
seven new kinds are added as enumerators; `DeterministicFaultInjector::apply`'s
`switch` gains no new *branches* for them -- they fall into the same
"annotate and pass through" case `kDelayed`/`kSequenceGap` already used,
proving by construction that market/execution stress needs nothing
different from a plain timing/sequence fault at this layer. Only
`kMissing` and `kDuplicated` (slice 11) ever change which records are
emitted; every stress kind, like `kSequenceGap`, is a label attached to an
untouched record.

**`magnitude` is the shared severity parameter; `kLatencySpike` reuses
`delay` instead.** Six of the seven kinds carry a caller-defined, scaled-
integer severity in `magnitude` (a widening factor, a spike size, a vanish
severity, a rejection-reason code, a fill-ratio numerator, a queue depth --
each kind documents its own units in the header, since there is no shared
unit across "spread" and "fill ratio"). `kLatencySpike` is the one
exception: "how much extra latency" is already exactly what `delay`
(a `common::Duration`) represents, so it reuses that field rather than
encoding a duration as an integer count of some implicit unit in
`magnitude`.

**Response stays entirely out of scope, for both requirements' full
acceptance text.** AEGIS-062's "risk responses are reproducible" and
AEGIS-063's "OMS/risk integration tests cover each fault" both name
subsystems `configs/architecture_rules.yaml` dates to M5
(`cpp-participant-risk`, `cpp-participant-oms`). What M2 delivers is the
half that can be honest before M5 exists: the fault signal itself,
attached to an exact record, deterministically -- so that whenever the
risk/OMS layers do exist, they have a fixed, already-proven-deterministic
input to write their response tests against, rather than needing to build
their own fault generator at the same time as their own response logic.

### Alternatives considered

- **A second injector class for stress kinds** -- rejected: the whole
  point demonstrated by putting them in the same `switch` case as
  `kSequenceGap` is that they need no different mechanism, only different
  labels; a second class would suggest a difference that does not exist.
- **Giving each stress kind its own typed parameter (a `Spread` type, a
  `FillRatio` type, ...)** -- rejected: none of these kinds has real
  market semantics at this layer (see Context), so a strongly-typed
  parameter would imply a precision this milestone cannot honestly claim.
  A documented, scaled integer is exactly as meaningful as the fault is at
  this layer, and no more.
- **Deferring AEGIS-062/063 entirely to M5** -- rejected: the owner's plan
  of record scopes the *injection mechanism* to M2 explicitly (the M2
  requirement coverage table lists both), with only the *response* residual
  to M5; deferring the mechanism too would leave M5 building both the
  fault generator and the response simultaneously, coupling two
  independently testable things.

### Consequences

- A future M5 risk/OMS consumer reads exactly the same
  `FaultInjectionResult` shape slice 11 defined; this addendum adds no new
  consumer-facing type.
- If a future milestone finds one of these seven kinds genuinely needs
  richer parameters than a scaled integer or a duration, that is a new ADR
  extending `FaultAnnotation`, not a silent repurposing of `magnitude`.

### Verification

- `tests/cpp/unit/test_market_stress_faults.cpp` -- a parameterized test
  proves the six `magnitude`-based kinds (`kSpreadWidening`,
  `kVolatilitySpike`, `kLiquidityVanish`, `kRejection`, `kPartialFill`,
  `kBackpressure`) all survive annotated with their configured magnitude
  and never drop a record; a dedicated test proves `kLatencySpike` carries
  its value in `delay`, not `magnitude`; a composition test proves stress
  and core (slice 11) fault kinds apply independently over different
  records in one call; and a determinism test proves repeated calls with
  the same rules agree.

## Owner approval

Authorized as part of M2 slice 11 (delayed/missing/duplicated/sequence-gap
faults, AEGIS-060/061) and M2 slice 12 (this addendum: market/execution
stress, AEGIS-062/063), both under the owner-approved M2 plan of record
(`experiments/plans/M2.md`, rev. 4) and the owner's slice 8-13 build-first
continuous-execution prompt, 2026-08-10.
