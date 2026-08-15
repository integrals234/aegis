# ADR-0023: OMS lifecycle, the mandatory risk seam, and the adapter/transport boundary

- Status: Accepted
- Date: 2026-08-12
- Requirement IDs: AEGIS-108, AEGIS-119
- Milestone: M3

## Context

AEGIS-108 names ten frozen states (`Created`, `RiskPending`, `Rejected`,
`Submitted`, `Acknowledged`, `PartiallyFilled`, `Filled`, `CancelPending`,
`Cancelled`, `Expired`) and requires that invalid transitions be rejected,
with tests covering every state. `cpp-participant-oms.may_depend_on` is
`[cpp-common, cpp-events, cpp-participant-risk]`
(`configs/architecture_rules.yaml`), and `cpp-participant-risk` is dated M5
and stays empty until then — so the mandatory `RiskPending` state has to be
real at M3 without a real risk policy existing yet.

AEGIS-119 ("environment-independent OMS... contract tests pass across
adapters") is the harder question. The obvious way to prove two adapters is
to build one that talks to the real M1 exchange — but `cpp-participant-oms`
has no dependency edge to any `cpp-exchange-*` layer, and MASTER_SPEC's first
immutable architecture principle states the exchange and the participant are
"separate systems connected only by versioned messages." An adapter that
constructs an `ExchangeNode` and calls it directly would be exactly the
production participant→exchange edge that principle forbids.

## Decision

**Ten states, one explicit transition table, rejection is a return value, not
an exception.** `OrderState` enumerates all ten; `is_legal_transition(from,
to)` is a pure function consulted by `OrderLifecycle::transition(next)`, which
applies the change and returns `true` on success or leaves state unchanged and
returns `false` on an illegal request. A request the caller cannot predict
will succeed — a fill racing a cancel, an exchange rejecting after
acknowledgement — is a legitimate business event; a genuinely impossible
transition (`Filled -> Submitted`) is a caller bug, and returning false rather
than throwing lets a caller distinguish "try something else" from "abort."

**The risk seam is mandatory and structurally present, with no policy
inside it.** Every path from `Created` to `Submitted` passes through
`RiskPending`; there is no legal transition that skips it.
`cpp/participant/oms/risk_gate.hpp` declares the pure-virtual `RiskGate`
interface (`decide(NewOrderCommand) -> RiskDecision`, verdict
approve/resize/reject) that a real M5 policy will implement. **No concrete
`RiskGate` ships in production code at M3** — the only implementation that
exists lives in test fixtures, explicitly named as a test double, so nothing
in the shipped library can be mistaken for risk logic that was never built.

**`ExecutionAdapter` and `ExecutionTransport` are defined over `cpp/events`
alone.** `ExecutionAdapter` is what the OMS calls: `submit`/`cancel`/`modify`
taking the existing `NewOrderCommand`/`CancelOrderCommand`/`ModifyOrderCommand`
types AEGIS-004 already put in `cpp/events` for exactly this purpose.
`ExecutionTransport` is the boundary an adapter delegates to: `send(Envelope)`
outbound, with inbound delivery left to a concrete transport's own mechanism.
**Neither interface, nor any type either one names, mentions an exchange
type.** This is what "environment-independent" in AEGIS-119's own title means
concretely, and it is exactly ADR-0009's reasoning about `cpp/events` applied
to the OMS: the interface can be satisfied by something that has never heard
of `ExchangeNode`.

**Where the real M1 exchange is actually reached: a test-only integration
harness, not a production adapter.** `tests/` is outside
`covered_roots` in `configs/architecture_rules.yaml`, so a harness there may
legally construct both a real `ExchangeNode` and a real participant stack and
wire an `ExecutionTransport` implementation directly to the exchange's
command/event interface. This produces no production dependency — the harness
is exercised by GoogleTest, never linked into `aegis_participant_run` — and it
is where AEGIS-109/110/111/114's "integration tests reconcile fills and
positions" against real M1 FIFO matching are proven. This work is scoped to
M3 slice 5 in the plan of record; this ADR records the boundary decision that
makes it possible without a forbidden edge, ahead of building it.

**One production adapter ships this slice: `TransportExecutionAdapter`.** It
implements `ExecutionAdapter` by encoding OMS intent to `Envelope`s and
handing them to an injected `ExecutionTransport&` — the seam M9's eventual
paper transport fills with a different concrete transport, unchanged. It owns
no responses of its own; they arrive asynchronously through whatever
transport is injected. A second adapter, `RecordedResponseAdapter` (driving
the OMS through committed response scripts for race and rejection scenarios
no live transport can be made to reproduce on demand), is scoped to slice 5
alongside the integration harness — both are needed together to prove
AEGIS-119's "across adapters," and neither is claimed as complete on its own.

## Alternatives considered

- **An in-library `SimulatedExchangeAdapter` calling `ExchangeNode`
  directly** — rejected: a production participant→exchange edge, forbidden by
  MASTER_SPEC immutable principle 1 and by
  `cpp-participant-oms.may_depend_on` naming no exchange layer.
- **Widening `cpp-participant-app` (or `cpp-participant-oms`) to depend on a
  `cpp-exchange-*` layer to make integration testing convenient** — rejected:
  the same violation one layer up; `tests/`'s exemption from `covered_roots`
  exists precisely so integration proof does not need a production edge.
- **Implementing M5's risk policy now so `RiskPending` "does something"** —
  rejected: not this milestone's requirement, and a real-looking policy that
  is not actually validated would be exactly the kind of unearned claim
  `docs/CV_CLAIMS_POLICY.md` and this project's completion rule forbid.
- **Throwing on an illegal transition instead of returning false** — rejected:
  a race condition producing a transition the caller could not have predicted
  is not the same class of error as a call-site bug, and the two need
  different handling.

## Consequences

- M5's `RiskGate` implementation plugs into the seam this ADR defines without
  any change to `OrderLifecycle` or the transition table.
- M9's paper adapter is a second `ExecutionTransport` implementation, not a
  new `ExecutionAdapter` — the OMS-facing contract does not change when the
  environment does.
- Slice 5's integration harness and `RecordedResponseAdapter` both consume
  exactly the `ExecutionAdapter`/`ExecutionTransport` shapes this ADR fixes
  now; neither requires revisiting this decision.

## Verification

- `tests/cpp/unit/test_order_lifecycle.cpp` — every one of the 10×10 state
  pairs is checked against the transition table; terminal states
  (`Rejected`, `Filled`, `Cancelled`, `Expired`) accept no outgoing
  transition; no path from `Created` to `Submitted` skips `RiskPending`.
- `tests/cpp/unit/test_execution_adapter.cpp` — `TransportExecutionAdapter`
  encodes `submit`/`cancel`/`modify` to the exact wire form
  `cpp/events/exchange_messages.hpp` already defines and hands it to the
  injected transport; a fake in-test transport is the only place a concrete
  `ExecutionTransport` exists outside production code.

## Owner approval

Authorized under the owner-approved M3 plan of record
(`experiments/plans/M3.md`) and its adapter-boundary correction, 2026-08-12.
