# Acceptance Gates

## Requirement status meanings

- `not_started`: no implementation.
- `in_progress`: active implementation, not claimable.
- `blocked`: dependency or evidence missing.
- `implemented`: code exists and local focused tests pass.
- `verified`: independent audit plus required test/report/demo evidence passes.
- `deferred`: owner-approved scope decision only; MUST items may not be silently deferred.

## Evidence rules

A requirement can be `verified` only if:
1. all evidence paths exist;
2. at least one evidence path is a test, benchmark, reproducible experiment, or demo artifact;
3. no evidence is only a TODO, screenshot without provenance, or prose claim;
4. the spec-auditor reviewed the implementation;
5. the requirement's stated acceptance criterion is satisfied.

## Milestone completion report

Every milestone report must include:
- requirement IDs implemented and verified;
- files changed;
- tests/commands run and exact results;
- known limitations and blocked IDs;
- architecture/ADR changes;
- benchmark or research methodology where relevant;
- reproducible demo command;
- explicit statement that later milestone features were not falsely marked complete.

## System-wide final gate

The project is not complete until:
- all MUST requirements are verified;
- clean build and test pass in a documented environment;
- deterministic replay checksum is stable;
- risk bypass tests pass;
- no production real-money path is enabled;
- benchmark and strategy claims are evidence-backed;
- end-to-end demo runs from data through exchange, strategy, risk, execution, analytics, and Decision Arena.
