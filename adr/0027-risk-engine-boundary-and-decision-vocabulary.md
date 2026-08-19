# ADR-0027: Risk engine boundary and decision vocabulary

- Status: Accepted
- Date: 2026-08-18
- Requirement IDs: AEGIS-120, AEGIS-137
- Milestone: M5

## Context

`cpp/participant/risk` is dated M5 in `configs/architecture_rules.yaml` and
must become non-empty this milestone. `cpp-participant-oms` has depended on
`cpp-participant-risk` since M3 (`oms::RiskGate` was declared there in
anticipation, ADR-0023) -- so the reverse edge, `cpp-participant-risk ->
cpp-participant-oms`, would be a dependency cycle, and `m5-architecture-
transition` authorises no such edge. `RiskEngine` therefore cannot implement
`oms::RiskGate` itself. Something still has to.

Separately, AEGIS-137 requires "every proposal has exactly one auditable risk
decision", and the M4 calendar-spread strategy is two-legged: a naive
per-order risk check would let one leg through and reject the other,
producing an unhedged position the strategy never proposed and no risk
decision that honestly describes what happened to the *proposal*.

## Decision

**The composition root owns the seam adapter, not the risk layer.**
`cpp/participant/app/risk_engine_gate.{hpp,cpp}` defines `RiskEngineGate :
oms::RiskGate`, translating `NewOrderCommand` into a call against
`risk::RiskEngine`. `cpp-participant-app` is the only layer permitted to see
risk, OMS, strategy and portfolio at once (ADR-0020), so it is the only
place this translation can legally live. `cpp/participant/risk` names no
`oms::` type anywhere in its public interface.

**Risk owns plain-old-data views, not references into other layers.**
`risk::OrderRequest`, `MarketQuote` and the position/reservation bookkeeping
in `RiskState` are all POD, populated by the composition root from whatever
it actually holds (`oms::NewOrderCommand`, `book::TopOfBook`,
`portfolio::Portfolio`). `RiskEngine` never depends on portfolio or
book-builder types; its own position tracking (`RiskState::apply_fill`)
duplicates a slice of what `Portfolio` already computes, deliberately, rather
than reach across a layer boundary `may_depend_on` does not grant.

**`evaluate`/`evaluate_proposal` are pure; `commit_proposal_decision`/
`decide_order` enforce.** `evaluate_proposal` runs every leg of a proposal
against the *current* committed state plus a working overlay that folds in
each already-evaluated sibling leg -- so a spread that would jointly breach a
portfolio-level limit rejects even though each leg passes in isolation. If
any leg rejects, every leg in the result reports `kReject`: no partial
information that could be read as "the other leg was fine".
`commit_proposal_decision` re-derives the same result and, only if it is not
a reject, arms one "pending leg" per order and appends the ONE terminal
`ProposalRiskDecision` this proposal_id will ever get. The composition root
calls `commit_proposal_decision` before submitting either leg to
`OrderManager`; if it rejects, `execute_leg` is called for neither leg, so
atomicity is structural, not a convention the composition root promises to
honour.

**`decide_order` is the seam's actual enforcement point, and it trusts
nothing it wasn't told.** `RiskEngineGate::decide` is called by
`OrderManager::submit_new_order` unconditionally (the state machine's own
structural guarantee, `order_state.hpp`) and forwards to
`RiskEngine::decide_order(instrument_id, side, quantity_units,
client_order_id, now)`. It matches against the pending legs
`commit_proposal_decision` armed, by `(instrument_id, side,
requested_quantity_units)`. An order with no match -- a bypass, or a caller
bug -- is rejected with `kUnexpectedOrder`: the structural defence
underneath AEGIS-120, verified at runtime by
`RiskEngineAuditInvariant.RejectedProposalReportsEveryLegRejectedAndArmsNoOrder`
(`tests/cpp/unit/test_risk_engine.cpp`) and, at the architecture level, by
the pre-existing `tests/unit/fixtures/arch_violation` negative fixture
(`cpp/participant/strategy/rogue.hpp`'s "Violation 2: a strategy holding a
gateway, bypassing risk and the OMS", already wired into
`scripts/ci_local.sh`'s `negative_gates` stage) -- both prove a
strategy-to-OMS edge is structurally rejected, one at the type-dependency
level and one at the seam's runtime behaviour.

**`RiskEngineGate::decide` is `const`, and that is honest, not a loophole.**
`oms::RiskGate::decide` is declared `const` (ADR-0023: the seam's contract
predates M5). `RiskEngineGate` stores `risk::RiskEngine*` -- a plain pointer,
not `const risk::RiskEngine*` -- as a member, so a `const RiskEngineGate`
method may still call a mutating method through that pointer: `const`
qualifies `this` (the adapter), not the engine object it was injected with
and does not own. This is the same discipline `OrderManager` already applies
to its own `adapter_`/`risk_gate_` pointer members.

**One canonical terminal decision per proposal; subordinate decisions per
order.** `ProposalRiskDecision` is the ONE record `commit_proposal_decision`
appends per `proposal_id`
(`RiskAuditLog::proposal_decision_count(proposal_id)` is asserted `== 1` by
test). `OrderRiskDecision` is subordinate -- one per order that actually
reached `decide_order` -- and always carries the `proposal_id`/`leg_index`
that produced it, so a reader can always trace an order decision back to the
one proposal decision that authorised it. Neither record is ever mistaken
for the other: they are distinct types, appended to distinct append-only
vectors in `RiskAuditLog`.

## Correction (M5 closure repair)

An independent risk-safety review found the ORIGINAL version of this
decision -- reserve exposure at `decide_order`, match a pending leg by
`(instrument_id, side, requested_quantity_units)` -- unsafe in two ways this
section retracts and replaces. The "`decide_order` is the seam's actual
enforcement point" paragraph above, and the "matches against the pending
legs... by `(instrument_id, side, requested_quantity_units)`" sentence in
particular, describe the ORIGINAL (defective) design; they are superseded by
this section, not deleted, so the record of what changed and why stays
legible.

**Defect 1: two individually-safe proposals could jointly breach a
cumulative limit.** Nothing counted a committed proposal's exposure until
its order physically reached `decide_order`. Two `+60`-unit proposals
against a `100`-unit position limit, committed before either's order
reached the seam, both passed preflight and jointly reserved `120`.

**Defect 2: an order could resolve to a look-alike armed leg from a
different strategy/proposal.** Matching by economics alone cannot
distinguish two proposals whose legs happen to share the same instrument,
side and quantity -- a strategy-B order could match strategy-A's armed leg
and inherit A's non-halted state, defeating AEGIS-135's strategy-scoped
kill switch.

A related gap surfaced in the same review: `commit_proposal_decision`
called `RiskAuditLog::record_proposal` unconditionally, even when the
per-leg idempotency check (`AEGIS-127`) rejected a replayed `proposal_id` --
so a replay appended a SECOND `ProposalRiskDecision` for the same
`proposal_id`, violating the "exactly one" invariant this ADR's "One
canonical terminal decision per proposal" section states, in a code path
the existing test suite exercised but never asserted the count against.

**The fix changes WHEN and BY WHAT KEY a leg's exposure and identity are
tracked, not what each cumulative control computes:**

- `commit_proposal_decision` now reserves ALL of an approved/resized
  proposal's legs' exposure IMMEDIATELY, keyed by
  `PendingLegKey{strategy_id, proposal_id, leg_index}` (no `client_order_id`
  exists yet at this point). Every cumulative control already reads
  `RiskState::reserved_units`/`all_reservations`, so a second proposal
  committed before the first's orders reach the seam now correctly sees the
  first's reservation -- closing Defect 1 without changing any control's own
  arithmetic.
- `commit_proposal_decision` also now checks `RiskAuditLog::
  find_proposal_decision(proposal_id)` FIRST: a `proposal_id` that already
  has a terminal decision returns that SAME decision, unconditionally,
  before `evaluate_proposal` even runs -- so a replay can never re-arm,
  re-reserve, or append a second record. `proposal_id` remains treated as
  globally unique (the composition root already embeds `strategy_id` into
  the string it generates, `strategy_id + "-" + sequence`); this is the
  existing contract this correction relies on, not a new, narrower one.
- The composition root registers each leg's future `client_order_id`
  (`OrderManager::next_client_order_id()`, peeked immediately before
  `submit_new_order`) against its exact `PendingLegKey` via
  `RiskEngine::register_pending_order_identity`, BEFORE the order reaches
  the OMS. `decide_order` resolves a `client_order_id` to its EXACT
  `PendingLegKey` through that registration -- never by searching economics
  -- closing Defect 2. It then verifies the order's own
  instrument/side/quantity agree with what was actually reserved
  (`kIdentityMismatch` if not -- a caller-bug signal, never a look-alike
  match, since identity resolution never searched by economics in the first
  place), revalidates mutable safety state that can have changed since
  commit (`revalidate_at_seam`: halts, connectivity, market
  staleness/collar, and every cumulative control -- excluding this leg's own
  already-counted reservation via an `EvaluationOverlay` populated with its
  negation, so the leg is counted exactly once, never twice, never zero
  times), and only then TRANSITIONS the existing leg reservation to be
  `client_order_id`-keyed for the ordinary fill/release lifecycle. It never
  reserves a second time.

Deliberately NOT revalidated at the seam: idempotency (this leg's own
dedupe key was already marked seen at commit; re-checking would reject
every order, since it is unconditionally "seen" by then) and the
order-count rate limit (this leg's own event was already recorded at
commit; re-checking would double-count a message that happened once).
Neither omission reopens a gap: idempotency's purpose (reject a REPLAYED
submission) is served by the proposal-level replay guard above, and the
rate limit's purpose (throttle a BURST of NEW admissions) was already
served when the leg was admitted at commit time.

`tests/cpp/unit/test_risk_engine_reservation_repair.cpp` reproduces both
original attacks (and the duplicate-proposal/mis-attribution defect) and
asserts the fixed behavior; every test in that file fails against the
engine as it stood before this correction.

## Alternatives considered

- **Risk implements `oms::RiskGate` directly.** Rejected: creates the
  `cpp-participant-risk -> cpp-participant-oms` cycle described above.
- **A single risk decision per order, no proposal-level concept.** Rejected:
  cannot express atomicity for a multi-leg proposal without either
  submitting legs speculatively (risking a naked position on partial reject)
  or duplicating cross-leg accounting inside the OMS seam, which has no
  visibility into sibling legs at all.
- **`OrderManager` forwards fill/terminal events to `RiskGate`.** Rejected as
  requiring an OMS change: `cpp/participant/oms/**` is outside M5's approved
  scope, and no approval covers it. Fill/terminal feedback is instead
  forwarded by the composition root, which already receives the raw
  exchange events it decodes for `OrderManager`. The one gap this leaves --
  `OrderManager` discards `ExecutionAdapter::submit`'s boolean return value,
  so a failed send is invisible to the OMS seam itself -- is closed without
  touching the OMS at all: `RiskReleasingExecutionAdapter` (below) is a
  decorator the composition root chooses, not a change to `OrderManager`.

## Consequences

- `cpp/participant/oms/**` is unmodified by M5 -- zero files, zero lines.
- `RiskEngine`'s own position/reservation bookkeeping is a second source of
  truth alongside `Portfolio`'s, kept in sync only by the composition root
  calling both `on_fill` (risk) and `apply_fill` (portfolio) from the same
  event. A caller that forwards one and not the other silently desynchronises
  them; nothing in either type detects this. Documented in
  `docs/LIMITATIONS.md`.
- A failed adapter submission releases its reservation automatically, through
  `app::RiskReleasingExecutionAdapter` -- a decorator over `ExecutionAdapter`
  the composition root installs, not a change to `OrderManager`
  (`cpp/participant/oms/**` stays unmodified). It sees the same
  `NewOrderCommand`, and therefore the same `client_order_id`, that
  `RiskEngineGate::decide` approved moments earlier, and calls
  `RiskEngine::release_reservation` only when `submit` returns `false`.
  `release_reservation` remains public for its own legitimate, separate use
  (manual reconciliation), but is no longer the *normal* path a caller must
  remember to invoke. `tests/cpp/unit/test_risk_fault_execution_stress.cpp`'s
  `BackpressureAutomaticallyReleasesTheReservationThroughTheNormalLifecycle`
  proves capacity returns with no manual call, and that a later order can
  use it again.

## Verification

- `tools/check_architecture.py` against the real
  `configs/architecture_rules.yaml`: passes, with `cpp-participant-risk`'s
  `may_depend_on` carrying no OMS/portfolio/book-builder edge.
- `tests/unit/fixtures/arch_violation` (unmodified): `check_architecture.py`
  still fails on it, including the strategy-holds-a-gateway violation.
- `tests/cpp/unit/test_risk_engine.cpp`'s `RiskEngineAuditInvariant` suite:
  exactly one `ProposalRiskDecision` per proposal_id; a rejected proposal
  arms no pending leg and any order that reaches `decide_order` anyway is
  rejected `kUnexpectedOrder`.
- `tests/cpp/unit/test_calendar_spread_risk_exchange_integration.cpp`: the
  same engine, through the same seam, against a real unmodified M1
  `ExchangeNode` -- reject means zero adapter submit, zero exchange order,
  zero portfolio change; resize means the exchange sees exactly the approved
  quantity.
- `tests/cpp/unit/test_risk_engine_reservation_repair.cpp` (M5 closure
  repair): two individually-safe concurrent proposals jointly breach a
  cumulative limit only until the SECOND one's commit, which now correctly
  rejects; a look-alike leg from another strategy cannot borrow its
  non-halted state; a replayed `proposal_id` never appends a second terminal
  decision; an order cannot be mis-attributed to the wrong proposal in
  either submission order; mutable safety state revalidated at the seam
  (kill switch, out-of-band exposure change) genuinely rejects without
  double-counting the leg's own reservation; a rejected two-leg proposal
  arms and reserves nothing for either leg; an identity/economics mismatch
  is rejected and releases its reservation.
- `aegis_participant_run --calendar-spread --stream
  tests/unit/fixtures/participant/calendar_spread_stream.jsonl --risk-config
  configs/risk/limits.json`: the real composition root, both proposals
  approve and fill, byte-identical across two runs; `--risk-config
  configs/risk/limits_reject_demo.json` rejects both proposals with zero
  orders, proving the corrected lifecycle end to end, not only inside unit
  tests.

## Owner approval

Implied by merged `m5-architecture-transition`/`m5-participant-app-integration`
(PR #13's activation policy); this ADR is filed alongside the implementation
it documents.
