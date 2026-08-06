# ADR-0001: Platform architecture and structural dependency enforcement

- Status: Accepted
- Date: 2026-08-06
- Requirement IDs: AEGIS-004, AEGIS-008, AEGIS-227
- Milestone: M0

## Context

`docs/ARCHITECTURE.md` states that exchange simulation and participant logic are
separate systems joined only by versioned messages, that a strategy emits
proposals and never calls an exchange or broker adapter, and that portfolio state
changes only from accepted execution and account events.

Every one of those statements is currently true because no code exists. The
question this decision answers is what keeps them true at M3, when a strategy,
a risk engine, an OMS and a portfolio all exist in the same process and the
shortest path from a strategy to a fill is a direct call.

Review does not keep them true. The violating edge is always small, always
locally reasonable, and always added on a Friday.

## Decision

The layer graph is declared in `configs/architecture_rules.yaml` and enforced by
`tools/check_architecture.py`, running in CI from the first commit. Four
independent things are read, because each catches what the others miss:

1. **Include and import edges.** C++ quoted includes are repository-root
   relative, so an include resolves to exactly one layer with no search-path
   ambiguity. Python imports resolve the same way.
2. **Namespace ownership.** A translation unit may only open namespaces its
   layer owns, so `aegis::exchange` code cannot appear inside a participant file
   that happens to include the right headers.
3. **CMake `target_link_libraries` edges.** An include-only checker misses link
   edges entirely, and a link edge is a real dependency.
4. **Banned constructs.** Parent-relative includes (which route around the
   graph), pybind11 and `Python.h` outside `cpp/bindings`, gateway and broker
   adapter headers outside the OMS, and file-scope mutable globals in the
   deterministic cores.

Two further rules keep the enforcement itself honest:

- **Total coverage.** Every source file under `cpp/`, `python/`,
  `decision_arena/` and `dashboard/` must map to exactly one declared layer. A
  package added in M3 fails the check rather than being silently unconstrained.
- **Declared population.** Each layer states the milestone it first carries
  sources from. Before that milestone it must be empty; from it, it must not be,
  so a rule about a layer cannot keep passing because the layer is empty.

The participant pipeline is forward-only: feed handler → book builder →
statistics → strategy → risk → OMS → portfolio. `strategy → oms`,
`strategy → gateway`, `strategy → cpp/exchange/**`, `oms → strategy` and
`portfolio → {strategy, risk, exchange}` are denied. Only the OMS may include a
gateway or adapter header (AEGIS-119, AEGIS-120).

Portfolio ownership follows from the same rule set: portfolio state is owned by
the portfolio module and changes only from accepted execution and account
events. Risk receives an immutable `PortfolioRiskSnapshot`; strategy receives a
read-only `PositionSnapshot`. Both cross the boundary by value or immutable
reference, and no handle to mutable portfolio state leaves the module. The
implementations arrive in M3 and M5; the edges that would permit a violation are
already denied.

There is no process-global mutable substrate. Loggers, metrics registries and
clocks are constructor-injected instances.

## Alternatives considered

**Review-only enforcement.** Rejected: the failure mode is a single small edge
added under time pressure, which is exactly what review is worst at catching
consistently.

**Advisory mode first, enforcing later.** Rejected: advisory output is how the
first violation lands unnoticed, and by the time enforcement is switched on there
is a backlog to grandfather.

**Symbol-name and vocabulary rules** (for example, banning the word `book` in
participant code). Rejected on owner direction: it constrains naming rather than
dependencies, and a determined violation just renames.

**Separate processes per subsystem.** Rejected for M0: it would enforce the
boundary absolutely, but at the cost of a serialization hop on the latency path
this project exists to measure, and it does not prevent an in-process violation
inside the participant.

## Consequences

- A layer edge must be declared before code can use it, which makes adding a
  dependency a visible decision rather than an import.
- The rules file is a second thing to maintain; total coverage means a new
  package cannot be added without touching it, which is the point.
- The mutable-globals check is a heuristic over text, not a C++ parser. It
  over-reports rather than under-reports; a false positive is dismissed in
  review, whereas hidden global state surfaces as replay nondeterminism much
  later and much more expensively.
- `cpp/exchange/**` and `cpp/participant/**` must stay empty until M1 and M3.

## Verification

- `tools/check_architecture.py` over the real tree, in CI and in
  `scripts/ci_local.sh`.
- `tests/unit/test_check_architecture.py` drives two committed fixture trees:
  `arch_ok` is legal, `arch_violation` commits one instance of every rule, and a
  test asserts all eight are reported.
- CI's `negative-gates` job fails if the checker ever accepts `arch_violation`.

## Owner approval

Recorded in the approved M0 plan (`experiments/plans/M0.md`, Part 4).
