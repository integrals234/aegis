# ADR-0003: Determinism and the canonical-output contract

- Status: Accepted
- Date: 2026-08-06
- Requirement IDs: AEGIS-005, AEGIS-003, AEGIS-233
- Milestone: M0

## Context

AEGIS-005 requires that identical event input and seed produce byte-identical
canonical output before performance work begins. At M0 there is no engine, so
there is nothing whose determinism could be claimed. The risk is that M0 closes
with a green determinism check that reads as "AEGIS is deterministic" — a claim
nobody made explicitly and everybody would infer.

## Decision

M0 establishes exactly one thing, stated in these words in the tool, the
evidence and the milestone report: **the harness detects nondeterminism.**

Canonical output at M0 covers *platform* records only — message envelopes with
opaque payloads, a metrics snapshot, a structured log stream, a resolved
configuration digest. No order, trade, book or sequencer record appears, because
none exists. Adding a fake one would be building M1 early and would make the
check appear to cover the engine.

Two mechanisms carry the decision:

- **Separate processes per run, with different `PYTHONHASHSEED` values.**
  Running twice in one process shares module state, interned strings and one
  hash seed, which hides the nondeterminism that actually bites later: output
  depending on dict ordering, address-derived hashes or process-wide caches.
- **A committed negative fixture.** `--producer nondeterministic` uses an
  unseeded generator — the most common way real nondeterminism enters a research
  pipeline — and the harness must flag it. `--expect-failure` inverts the result
  and itself fails if the fixture ever becomes stable, so the fixture cannot
  quietly stop proving anything.

A single run is rejected: it cannot disagree with itself.

## Alternatives considered

**Claim system determinism at M0.** Rejected: there is no system.

**Run the producer twice in one process.** Rejected: cheaper and blind to the
category of nondeterminism that matters.

**Hash a wall-clock-stamped artifact.** Rejected: it would fail every run, so
the clock would have been frozen for the test only — which tests the frozen
clock rather than the producer.

**Defer the harness to M1.** Rejected: the harness must exist before the code it
will judge, or the first engine output becomes the baseline by default.

## Consequences

- The determinism claim in M0's report is narrow and will read as
  under-claiming to anyone who does not read the residual. That is the intended
  direction of error.
- AEGIS-005 closes M0 `implemented` with a registered obligation: real event
  input at M1, and a hard gate before M8 begins optimization.
- Every artifact that enters canonical output must be byte-stable, which is why
  the logger takes an injected clock and the metrics snapshot sorts its keys.

## Verification

- `tools/determinism_check.py --runs 2 --seed 42`.
- `tools/determinism_check.py --producer nondeterministic --expect-failure`.
- `tests/replay/test_determinism_harness.py`, including a test asserting that
  the committed evidence still carries the narrow wording, so the claim cannot
  quietly grow.
- Committed artifacts: `experiments/evidence/AEGIS-005/summary.json`,
  `experiments/evidence/AEGIS-005/run1.hash`, `run2.hash` and
  `canonical_output.txt`.

## Owner approval

Recorded in the approved M0 plan (`experiments/plans/M0.md`, Part 5, AEGIS-005).
