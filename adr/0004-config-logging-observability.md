# ADR-0004: Configuration, logging and observability contracts

- Status: Accepted
- Date: 2026-08-06
- Requirement IDs: AEGIS-231, AEGIS-232, AEGIS-238
- Milestone: M0

## Context

Three substrate services are needed by everything built after M0, and each has a
characteristic way of becoming untrustworthy: a configuration that is not the
one that ran, a log that cannot be joined to the run that produced it, and a
metric that reads zero because nothing writes it.

## Decision

**Configuration.** `configs/schemas/config.v1.json` is the single statement of
what a valid configuration is. Both the Python reference loader and the C++
loader read that file at runtime; neither transcribes the rules. `config_version`
is mandatory and enumerated, and an unknown version is rejected rather than
interpreted under today's field meanings. Precedence is defaults < file <
environment < CLI, resolved once, with per-leaf provenance recorded. Both
loaders report every problem at once, each naming its field path.

The **resolved** configuration is what gets hashed into the experiment manifest,
not the file: environment and CLI overrides change what actually ran.

**Logging.** JSON Lines validated against
`configs/schemas/log_record.v1.json`. Every record carries `experiment_id` — the
same field the envelope carries, so a log line and an event join without a
lookup table — plus an optional `correlation_id` for one causal chain. The clock
is injected and a per-logger sequence breaks timestamp ties, so a fixture run
twice produces identical bytes and can be determinism-harness input. Field values
are redacted by key name as the record is built: a logger is the most common way
a credential reaches disk, so the check lives where the record is built rather
than in a review convention.

**Observability.** An instance-owned registry of counters, gauges and
histograms, read through immutable pull-based snapshots so an observer cannot
mutate what it observes. Histograms report p50/p95/p99/p99.9 by nearest rank —
an interpolated p99.9 reports a latency no request experienced, and
`docs/BENCHMARK_POLICY.md` forbids quoting the mean alone. Health has three
states because "not healthy" is two different operational answers, and the
overall state is the worst check rather than an average.

**No domain metric is pre-registered.** `queue_depth`, execution latency and
risk status arrive with their producers in M1, M3 and M5. A gauge nobody writes
reads zero, and an operator believes the zero — a never-written gauge is
indistinguishable from a genuinely empty queue exactly when the difference
matters.

All three are instances. No module-level default logger, registry or clock: a
process-global would be mutable state reachable from every book partition.

## Alternatives considered

**Two schemas, one per language.** Rejected: they drift, and the drift surfaces
as a run that behaved differently from the configuration describing it.

**Tolerating an unknown `config_version`.** Rejected: silently reinterpreting a
document changes what the run does without changing what the file says.

**Python's `logging` module.** Rejected: its global registry and handler
hierarchy are precisely the process-global state this decision excludes, and
structured output through it is a formatter bolted onto a text-first design.

**Pre-registering the full AEGIS-238 metric set now.** Rejected: it would make
M0 look observable. The requirement is satisfied progressively, with each metric
appearing alongside the code that feeds it.

**Push-based metrics.** Deferred: pull-based snapshots are simpler to make
deterministic, and nothing yet needs a push.

## Consequences

- Adding a configuration field means editing one schema, and both loaders pick
  it up.
- Every component that logs takes a logger and a clock.
- `docs/DEFERRED_VERIFICATION.md` carries AEGIS-238 until queue depth, latency
  and risk status have producers.
- The C++ JSON Schema validator implements a documented subset and reports any
  keyword outside it. A validator that skips what it does not understand accepts
  documents the schema meant to reject.

## Verification

- `tests/unit/test_config_validation.py` and `tests/cpp/unit/test_config.cpp`
  drive one corpus (2 valid, 10 invalid) through both loaders, asserting on
  message content.
- `tests/unit/test_structured_logging.py`, `tests/cpp/unit/test_logging.cpp`.
- `tests/integration/test_metrics_registry.py`, including a test asserting no
  domain metric is pre-registered.

## Owner approval

Recorded in the approved M0 plan (`experiments/plans/M0.md`, Part 6, ADR-0004).
