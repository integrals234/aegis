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
against the *current* committed state plus a working overlay -- so a spread
that would jointly breach a portfolio-level limit rejects even though each
leg passes in isolation. **Correction (see "Correction 2" below):** this
overlay originally folded in only each *already-evaluated* sibling leg (a
growing prefix), which could not see a later leg's exposure REDUCTION; it
is now built from ALL of the proposal's legs before any leg's cumulative
controls are judged, so every leg sees the SAME final combined projection
regardless of leg order. If any leg rejects, every leg in the result
reports `kReject`: no partial information that could be read as "the other
leg was fine".
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

## Correction 2 (M5 closure repair): proposal-atomic final-overlay
evaluation and seam revalidation

A second risk-safety review, of the correction above, found it still
permitted a naked leg through a different door: risk-CAPACITY atomicity
(the reservation fix) is not the same thing as risk-DECISION atomicity, and
this engine only had the former.

**Defect A: `evaluate_proposal` judged each leg against a PREFIX overlay.**
Legs were folded into the overlay in iteration order, so leg *i* only ever
saw legs `0..i-1`. A later leg that REDUCES exposure (a closing or rolling
trade) was invisible to an earlier leg's own cumulative check. Reproduced
(`ProposalAtomicSeamRevalidation.N6ExposureReductionRejectsAtomicallyAtCommit`):
background exposure in two instruments, a two-leg proposal whose leg 0 ADDS
exposure to a third instrument and leg 1 REDUCES one of the background
instruments. Leg 0's preflight, evaluated before leg 1's reduction was
staged, saw a denominator that had not yet shrunk, understating its own
concentration share and approving a proposal whose TRUE combined share
(computed after leg 1's reduction) was well over the limit.

**Defect B: `decide_order`/`revalidate_at_seam` judged each leg
independently at the seam.** Even with Defect A fixed, mutable state can
still change between commit and seam arrival. The prior correction's
per-leg `revalidate_at_seam` was individually correct for the leg it was
called on, but nothing stopped one sibling from failing that per-leg check
while another sibling, checked moments later against by-then-different
state, passed -- a naked leg, produced not by a wrong formula but by
treating an atomic proposal's legs as independently revalidatable.

**The fix makes evaluation, not just reservation, proposal-wide, reusing
the SAME `EvaluationOverlay` abstraction rather than adding a second
accounting path:**

- `evaluate_proposal` now runs in two phases. Phase A resolves every leg's
  non-cumulative admission (halts, connectivity, market state,
  idempotency/rate-limit, order-quantity-cap/volatility sizing -- none of
  which depend on a sibling's exposure, so leg-order evaluation there is
  honest, not a prefix approximation) and builds ONE final combined
  overlay from every leg's own resolved quantity. Phase B judges each
  leg's cumulative controls (`check_cumulative_controls`, a new shared
  helper `evaluate_leg` and `revalidate_at_seam` also call, so there is
  exactly one implementation of control groups 8-12, not three) against
  that SAME final overlay, with only that leg's own contribution excluded
  first -- the identical exclude-then-readd technique the first correction
  already used at the seam. A proposal whose true combined effect is
  unsafe now rejects atomically at commit, before anything is armed or
  reserved, regardless of leg order.
- (M5 closure repair, N8: historical -- **at Correction 2**, `decide_order`
  no longer called `revalidate_at_seam` per leg. It instead first called
  `ensure_proposal_seam_revalidated`, which ran -- exactly once per
  proposal, cached in `proposal_seam_state_by_id_` -- a single
  revalidation pass over EVERY still-pending leg of that proposal against
  current mutable state. The first leg found unsafe condemned the WHOLE
  proposal: every one of its still-pending legs' reservations was released
  immediately, and the cached `kRejectedAtSeam` outcome was what every
  subsequent `decide_order` call for that proposal's legs consulted --
  never a fresh, independent check. A subtlety the implementation had to
  get right: the leg whose arrival TRIGGERED revalidation had to still be
  present in `pending_legs_` at that moment (erased only after, not
  before) or a single-leg proposal would be invisible to its own
  revalidation and vacuously "pass." Neither `ensure_proposal_seam_revalidated`
  nor `proposal_seam_state_by_id_` exist in the engine as of Correction 3;
  see below.)

**Superseded in part by "Correction 3" below.** This paragraph originally
claimed unqualified whole-proposal atomicity. A further review showed the
claim was too strong for the design as it then stood: caching the seam
verdict from the FIRST leg's arrival left later legs unchecked against
kill switches, connectivity and staleness, and `kIdentityMismatch` still
rejected a single leg while a sibling remained executable. Correction 3
qualifies the guarantee to the release epoch and makes it true.

**What this does NOT guarantee: atomic EXCHANGE execution.** Once risk
approves both legs, a transport or exchange failure on ONE leg after
submission can still leave the other filled alone -- this system has no
basket/atomic multi-leg execution primitive, and this repair does not add
one. Risk atomicity: yes. Exchange/broker multi-leg atomicity: no, not
claimed. (`docs/LIMITATIONS.md`.)

`ProposalAtomicSeamRevalidation.ALateStateChangeMakingOnlyOneLegUnsafeRejectsTheWholeProposalAtTheSeam`
proves Defect B's fix directly and generically (a position limit, not
concentration): an out-of-band fill makes leg A's own instrument unsafe
while leg B's instrument and control are completely untouched; submitting
B FIRST still rejects the whole proposal, not just A.
`CalendarSpreadRiskExchangeIntegration.ConcentrationBreachRejectsBothLegsAtomicallyInTheRealSeamNeverJustOne`
proves the same property through the real composition root.

## Correction 3 (M5 closure repair): the proposal release epoch

An independent risk-safety review of Correction 2 reproduced two remaining
defects, both from the same mistake: the whole-proposal seam revalidation
was correct in shape but began at the wrong MOMENT -- when the first
constituent reached `decide_order`, i.e. when that constituent was already
being released.

**Blocker A -- over-cached safety.** Caching the entire revalidation meant
every LATER constituent re-checked nothing. Differential probes against the
parent commit confirmed a global kill switch, an exchange disconnect and a
hard position-limit breach arriving between two legs each stopped leg 1
before Correction 2 and stopped nothing after it. That contradicted
AEGIS-135's "prevents new orders" and this ADR's own claim that the seam is
where a late-breaking change bites.

**Blocker B -- identity mismatch stranded a sibling.** `decide_order`
released only the mismatched leg's reservation, leaving a correctly-staged
sibling free to execute alone.

**The naive repair is wrong and was deliberately not taken.** Caching the
cumulative verdict while re-running hard safety per leg merely relocates
the hazard: leg 0 released, state changes, leg 1 rejects, naked leg. Any
design in which a per-leg check can fire AFTER a sibling is executable can
produce a mixed verdict.

**Decision: move the epoch earlier, to a point where nothing is released.**

1. `commit_proposal_decision` reserves and arms every leg (Correction 1).
2. The composition root builds all N constituent commands and calls
   `RiskEngine::stage_proposal_release` with every one of them --
   `client_order_id` (predictable: `OrderManager` assigns consecutively
   from `next_client_order_id()`, and it is the sole mutator), leg index,
   and the economics that order will carry. Staging is NOT permission; the
   proposal sits in `kStaging` and nothing is executable.
3. `RiskEngine::authorize_proposal_release` performs ONE fresh
   whole-proposal authorization: every committed leg must have a staged
   constituent whose economics match it (closing Blocker B structurally,
   before any release), and every leg must pass `revalidate_at_seam`
   against CURRENT state -- halts, connectivity, staleness/collar, and
   every cumulative control over the final combined overlay. Either ALL
   constituents become authorized (`kAuthorizedForRelease`) or NONE do
   (`kRejectedAtRelease`, every reservation released, every leg
   invalidated, permanently). The decision is terminal and idempotent: a
   proposal has at most one, recorded as a `ProposalReleaseRiskDecision`.
4. Individual `decide_order` calls then CONSUME that authorization. They
   check only what cannot split a proposal's verdict: exact
   `PendingLegKey`, exact immutable economics (a defensive backstop --
   already validated at step 3), and not-already-consumed. They compute no
   second proposal-level safety verdict, which is precisely what closes
   Blocker A without recreating the naked leg.

An unauthorized constituent is rejected `kProposalNotAuthorized`; an
incompletely staged proposal is rejected `kIncompleteProposalStaging`.
Both are fail-closed defaults, so a caller that skips the epoch gets
nothing executable rather than silently bypassing it.

**Kill-switch timing semantics, stated explicitly.** A kill switch tripping
BEFORE the release epoch rejects the whole proposal and releases zero
orders. A kill switch tripping AFTER authorization does NOT retroactively
reject a remaining constituent: the proposal is already one authorized risk
action, and splitting it now is the very hazard this design prevents. The
switch blocks all SUBSEQUENT proposals (proven by test) and the existing
emergency-cancel path handles live orders. This is a deliberate choice,
pinned by
`ProposalReleaseEpoch.AKillSwitchAfterAuthorizationDoesNotSplitTheProposalsVerdict`
so a future refactor cannot silently restore the per-leg conflict.

**What is and is not claimed (M5 closure repair, R4: corrected, narrower
statement).** RISK-DECISION atomicity: before any constituent of a proposal
is released, AEGIS makes one all-or-none authorization, and afterwards
`decide_order` computes no further proposal-level safety verdict -- no
halt, connectivity, staleness or cumulative-control question is asked a
second time. One integrity backstop remains: if an order actually submitted
disagrees on instrument/side/quantity with the exact economics that
constituent was staged and authorized under, THAT ONE ORDER is rejected
`kIdentityMismatch` while its siblings are unaffected. This is not a second
risk judgment -- it consults no mutable safety state, so it cannot recreate
the naked-leg hazard the release epoch prevents -- it only refuses to
execute something other than what was actually authorized. The previous
wording here ("never produces a contradictory per-leg risk verdict for that
proposal") was an overclaim: this backstop IS a per-leg outcome that can
differ from a sibling's. Verified against the real composition root
(`cpp/participant/app/participant_run.cpp`): the staged and submitted
economics are read from the same `StrategyLeg` fields, so the backstop is
confirmed NOT to fire on that path today; it exists for any caller that
drives this seam directly. NOT exchange/broker execution atomicity: after
authorization the transport or exchange can still accept one leg and fail
another, because AEGIS has no basket/atomic multi-leg execution primitive.
That residual is execution risk, not a contradictory risk verdict
(`docs/LIMITATIONS.md`).

**Limit semantics (the reviewer's Finding 3), stated rather than implied.**
Cumulative limits are evaluated against the proposal's FINAL NET PROJECTED
state -- the portfolio transition the proposal intends -- not against every
sequential intermediate state its legs could pass through during non-atomic
execution. A proposal that adds exposure on one leg and reduces it on
another is judged on the net result; if the adding leg fills before the
reducing leg is sent, true instantaneous gross exposure can exceed what
risk authorized. This follows from evaluating a multi-leg proposal as one
intended portfolio transition, and M5 does not claim worst-path or basket
execution-risk protection. Documented in `docs/LIMITATIONS.md`.

## Correction 4 (M5 closure repair): input integrity and reservation lifecycle

An independent risk-safety review of Correction 3's release epoch confirmed
Blockers A and B closed and the epoch itself sound against 18 attack
categories, but found one new blocker and several residuals in the layer
below it: the epoch's safety only means something if the QUANTITIES it
reserves and revalidates are themselves honest, and if a proposal that was
safely authorized cannot be poisoned, stranded, or silently outlived by
stale sizing.

**R1 (BLOCKER) -- `quantity_units <= 0` was accepted as legitimate input.**
`Side` already carries direction (`kBuy`/`kSell`); a negative quantity is
not a valid same-magnitude order on the opposite side, it is malformed
input. Left unvalidated, a negative quantity drove a NEGATIVE reservation
that then SUBTRACTED from every cumulative control's projected exposure --
reproduced exactly as the reviewer described: a 100-lot position cap, a
committed leg of `-100`, then a legitimate 150-lot order that the cap
should reject sails through because the projection reads `0 + (-100) + 150
= 50`. **Fix:** a new control group 0, `check_quantity_validity`, runs
FIRST in `evaluate_leg` and in every leg of `evaluate_proposal`'s Phase A --
before any other control reads `quantity_units` at all -- rejecting
`kInvalidQuantity` before any reservation, dedupe key, rate-limit token or
armed leg is created. Multi-leg proposals reject atomically: one invalid
leg rejects the whole proposal, arming and reserving nothing for any leg.

**R2 (CONCERN) -- an over-fill (or a duplicate fill report) could drive a
reservation negative.** `RiskState::reduce_reservation` subtracted the
reported fill unconditionally; a fill larger than what remained reserved
crossed zero and kept going, producing the same negative-reservation
under-counting hazard as R1 from a different direction. **Fix:** the
reduction is capped at the reservation's own remaining magnitude -- it can
shrink to exactly zero, never past it. No new anomaly-tracking framework;
the existing per-reservation state is simply clamped.

**R3 (CONCERN) -- an authorized-but-never-consumed proposal leg had no
recovery path.** Nothing but `decide_order` ever released a leg-keyed
reservation created by `commit_proposal_decision`+`authorize_proposal_release`;
a caller that authorized a proposal and then, for a reason outside
`RiskEngine`'s own visibility, never submitted one or more legs would
strand that capacity forever. **Fix:** `abort_proposal_release(proposal_id,
reason, now_nanos)` releases every still-unconsumed leg reservation of a
proposal and moves it to a new terminal `kAborted` state, idempotent and
terminal like `authorize_proposal_release`, and never rolls back a leg that
already consumed its authorization (that leg is live, or terminal, through
the ordinary OMS/fill lifecycle). No current call site exists in the
calendar-spread demo -- `execute_leg` never partially fails in a way that
leaves a sibling leg's authorization stranded -- so this is a lifecycle
primitive available to a caller that needs it, exercised directly by
`ProposalAbort` in `tests/cpp/unit/test_risk_proposal_release_epoch.cpp`.

**R5 (CONCERN) -- `stage_proposal_release` overwrote the canonical
`strategy_id` unconditionally.** A staging call naming a DIFFERENT
`strategy_id` for a `proposal_id` that already had a canonical one (set at
`commit_proposal_decision`) silently rewrote `ProposalReleaseRecord::strategy_id`
-- corrupting the AEGIS-137 audit trail's attribution of who actually
committed the proposal. **Fix:** canonical attribution is immutable. A
disagreeing staging call binds none of its own identities and marks the
record `attribution_mismatch`; `authorize_proposal_release` rejects the
whole proposal `kIdentityMismatch` the first time it observes the flag.
Deliberately fail-closed and permanent: once a mismatch has touched a
`proposal_id`, even a subsequent, correctly-attributed re-stage from the
legitimate strategy does not un-poison it, because the engine cannot tell
after the fact which call was the bogus one.

**R6 (CONCERN) -- the volatility HARD-REJECT safety gate was not
re-evaluated at release, unlike every other hard safety control.**
`revalidate_at_seam` deliberately does not re-run order sizing (the
quantity a proposal was authorized under must stay frozen), but the
hard-reject branch of `resolve_effective_quantity` is a SAFETY GATE, not a
sizing choice, and had been swept into the same "do not re-run" bucket by
mistake. **Fix:** `check_volatility_hard_reject` -- the hard-reject
condition alone, never the resize branch -- is re-run fresh inside
`revalidate_at_seam`, so a proposal safely sized while volatility was calm
but whose reference instrument has since spiked past the hard-reject
multiple is rejected at release, exactly like a kill switch or a
disconnect.

**R4 (CLAIM CORRECTION, no code change) -- "never a mix" was an
overclaim.** See the corrected "What is and is not claimed" paragraph
above: the R4 finding is that the identity-mismatch defensive backstop in
`decide_order` (Correction 3, step 4) IS a per-leg outcome that can differ
from a sibling's, even though it consults no mutable safety state and is
verified not to fire on the real composition root's own path today. The
claim is corrected, not the backstop weakened.

**Disposition of R8 and R9 (not fixed in this correction).** R8
(`AlwaysApproveRiskGate` reachable via `aegis_participant_run --fixture`,
pre-existing from M3, not reachable in the calendar-spread flow) and R9
(risk state -- kill switches, latches, reservations, proposal-release
records -- does not survive a process restart, and `docs/LIMITATIONS.md`
previously disclosed this only for idempotency) are both outside this
correction's declared surface (input integrity and reservation lifecycle
for the release epoch). R9 is closed as a documentation gap: the existing
idempotency-only disclosure is extended to name every kind of risk state
that does not survive a restart. R8 is deliberately NOT fixed here -- it
touches the `--fixture` CLI path's own composition, not the calendar-spread
release epoch -- and remains flagged as a known, disclosed gap for a future
turn to decide, rather than silently broadening this one.

## Correction 5 (M5 closure repair): terminal audit state and risk boundary validation

An independent re-review of Correction 4 confirmed the release epoch
itself still sound (15/15 sampled attack categories, unchanged), but found
one new blocker and five narrower residuals, all confined to the same
input-integrity/reservation-lifecycle surface.

**N1 (BLOCKER) -- `kAborted` was not terminal to
`authorize_proposal_release`.** Its terminal-state check listed
`kAuthorizedForRelease | kRejectedAtRelease | kCompleted`, omitting
`kAborted`. Calling `authorize_proposal_release` again AFTER a deliberate
`abort_proposal_release` therefore fell through, found `leg_keys` empty
(the abort had already released them), and called `reject_proposal_release`
-- overwriting the deliberate abort's terminal state and reason with a
spurious `kUnexpectedOrder` rejection, and appending a THIRD
`ProposalReleaseRiskDecision` for one proposal. Reproducible with the
ordinary authorize-abort-authorize sequence; no attack construction
needed. **Fix:** `kAborted` added to the terminal check.

**This also forced a truthful correction to the audit model itself,
rather than a code change that would have distorted it to preserve a
false invariant.** `RiskAuditLog::proposal_release_decision_count`'s own
doc previously claimed "AEGIS-137's release invariant asserts this is
never above 1" -- already false before N1's fix, for the perfectly
legitimate authorize-then-abort sequence (two real, distinct lifecycle
events: one authorize, one abort). The corrected model, stated precisely:
a committed proposal has AT MOST ONE authorize-or-reject transition
(whichever happens first is terminal for that pair), and MAY separately
have exactly one later abort transition if it was authorized and then
deliberately abandoned. `ProposalReleaseRiskDecision`'s own doc, and
`authorize_proposal_release`/`abort_proposal_release`'s header docs, now
state this. What genuinely never exceeds one, for any `proposal_id`, is
`RiskAuditLog::proposal_decision_count` (`ProposalRiskDecision`) --
AEGIS-137's frozen "exactly one" requirement belongs there alone, and was
never actually about the release-lifecycle count. The type
`ProposalReleaseRiskDecision` itself is unrenamed (a contained fix,
matching this correction's declared scope): its doc now describes it
explicitly as one event in a release LIFECYCLE, not a single terminal
"decision".

**N2 -- `RiskEngine` could still manufacture a non-positive approved
quantity, from the configuration side.** R1 (Correction 4) validated the
REQUESTED quantity; a malformed configured `OrderQuantityLimit` (a
non-positive `max_order_quantity_units` with `resize_on_breach == true`)
let `resolve_effective_quantity` itself resize a request down to a
non-positive `effective_quantity`, which `commit_proposal_decision` would
then reserve and `decide_order` would then approve. **Fix, two layers, as
directed rather than relying on one:** `app::load_risk_limits_config`
rejects a non-positive configured `max_order_quantity_units` at load time
(`std::runtime_error`, matching this loader's existing error-handling
convention); `RiskEngine::check_approved_quantity_postcondition`
(`ReasonCode::kInvalidLimitConfiguration`) is a defense-in-depth
postcondition, called after every `resolve_effective_quantity`, that
rejects regardless of how the config was constructed -- proven directly by
constructing a malformed `RiskLimitsConfig` programmatically, which the
loader-side validation cannot see.

**N3 -- a fill quantity was not validated the same way a request/approved
quantity now is.** `RiskEngine::on_fill` accepted any
`fill_quantity_units`, including negative or zero. A negative fill moved
`RiskState::apply_fill`'s confirmed position in the wrong direction --
silently poisoning every cumulative control that reads position, the
exact family of hazard R1/N2 close for quantity elsewhere in this engine.
**Fix:** `on_fill` requires `fill_quantity_units > 0`; an invalid fill
mutates neither position nor reservation, exactly as if no fill event had
been reported at all. A genuinely positive over-fill still saturates the
reservation at zero exactly as R2 (Correction 4) established -- this is
additive to R2, not a change to it.

**N4/N5/N6 -- the release lifecycle's identity boundary was inconsistent
across its three mutating entry points.** R5 (Correction 4) made
`stage_proposal_release` fail closed on a `strategy_id` mismatch, but left
two gaps in the same boundary: `authorize_proposal_release` accepted ANY
caller's `strategy_id` without comparing it to the proposal's canonical
one (N4) -- a caller could authorize, or read the release decision for, a
proposal it never committed, merely by knowing its `proposal_id`.
`abort_proposal_release` took no `strategy_id` argument at all (N6) --
the same gap, for abort. And underlying both: `commit_proposal_decision`'s
own unconditional `record.strategy_id = strategy_id` assignment
contradicted Correction 4's own documented "set exactly once, never
overwritten" claim (N5) -- true in effect only because nothing else could
reach the assignment first in the tested call order, not because the code
actually prevented a pre-commit caller from racing to set it.

**Fix, addressing the root cause rather than patching each symptom
separately:** canonical strategy attribution now originates in EXACTLY
ONE place -- `commit_proposal_decision` -- via a new
`ProposalReleaseRecord::committed` flag, set `true` only there, alongside
`strategy_id`. `stage_proposal_release`, `authorize_proposal_release` and
`abort_proposal_release` all look up their record with `find()`, never
`operator[]`, and treat `committed == false` (including "no record at
all") identically to "unknown proposal": no persistent state is created,
so none of the three can ever be the FIRST call to establish ownership,
and a pre-commit call against any of them leaves nothing for a later
legitimate commit to overwrite or be poisoned by. `authorize_proposal_release`
and `abort_proposal_release` both now require their `strategy_id`
argument to equal the proposal's canonical `record.strategy_id`: a
mismatch on authorize fails the whole proposal closed
(`kIdentityMismatch`, same semantics as a staging mismatch); a mismatch on
abort is a pure no-op that mutates and audits nothing, leaving the
canonical proposal completely unaffected.

**N7 (documentation only) -- a false "can never disagree" claim.**
`check_volatility_hard_reject`'s doc claimed a fresh release-time check
"can never disagree with the one taken at commit for the same volatility
reading." False for a misconfigured `hard_reject_multiple < 1.0`:
`resolve_effective_quantity` only evaluates its hard-reject branch inside
an `realized > target_volatility` guard, so a sub-1.0 multiple puts the
hard-reject threshold BELOW the resize threshold, and commit-time sizing
can approve a quantity release-time hard-reject immediately rejects, with
volatility unchanged in between. No frozen requirement constrains this
configuration value, so it is not validated at load time (unlike N2's
`max_order_quantity_units`, which IS constrained by "reject or resize" --
AEGIS-121's own frozen acceptance). **Fix:** the false claim is corrected;
`VolatilityReductionConfig`'s own doc states the intended domain
(`hard_reject_multiple >= 1.0` when `target_volatility > 0`) without
adding a runtime validator for it.

**N8 (documentation only) -- two surviving present-tense references to a
retired mechanism.** This ADR's own "Correction 2" section, and
`docs/BUILD_STATE.md`'s "Follow-up correction 2", both described
`ensure_proposal_seam_revalidated` (retired at Correction 3) in the
present tense. **Fix:** reworded to past tense with an explicit note that
the mechanism does not exist in the engine as of Correction 3, in both
files -- the historical narrative itself is not rewritten, only its tense.

## Correction 6 (M5 closure repair): authorize proposal identity isolation

**Superseded: the N4 fix description above.** Correction 5's own N4/N5/N6
paragraph, two sections up, said a caller `strategy_id` mismatch on
`authorize_proposal_release` "fails the whole proposal closed
(`kIdentityMismatch`, same semantics as a staging mismatch)". An
independent review found this description was itself the bug: it fails
the whole proposal closed by calling `reject_proposal_release`, which
releases every reservation, erases every armed leg, and latches
`kRejectedAtRelease` -- permanently. Two consequences, both reproduced
without any attack construction beyond the ordinary API:

1. **Availability/risk-budget theft.** A caller with NO relationship to a
   proposal could call `authorize_proposal_release(attacker_id, victim_
   proposal_id, ...)`, and while the attacker gained no execution (the
   proposal never reached `kAuthorizedForRelease` under its identity), the
   victim's proposal was destroyed and its reserved risk budget freed --
   available for the attacker's OWN next proposal to consume.
2. **Terminal-decision disclosure.** The terminal-state check ran BEFORE
   the identity check, so a wrong-strategy call against an
   already-`kAuthorizedForRelease`/`kRejectedAtRelease`/`kAborted`/
   `kCompleted` proposal received that proposal's REAL stored decision --
   state, reason code and reason string -- not a denial.

**The governing principle, stated precisely this time:** a wrong-strategy
`authorize_proposal_release` call is an UNAUTHORIZED QUERY, not a risk
rejection of the proposal it names. It must never mutate that proposal's
state, and it must never disclose that proposal's real decision.

**Fix.** The identity check moved to run FIRST -- immediately after
resolving the record and before any inspection of `record.state`, before
the terminal-state lookup, and before anything is mutated. On a mismatch,
`authorize_proposal_release` returns a single synthetic
`ProposalReleaseDecision{kRejectedAtRelease, kIdentityMismatch, <generic
reason text>}` constructed inline -- it never touches `record`, never
calls `reject_proposal_release`, never appends a
`ProposalReleaseRiskDecision`, and is IDENTICAL regardless of whether the
named proposal is unknown, staged, authorized, rejected, aborted or
completed. It carries no information beyond what the caller itself
supplied. The canonical owner's own subsequent call is completely
unaffected -- proven by test, not merely asserted: capturing release
state, reservation totals, leg-reservation count and release-audit count
before an attacker's call, and asserting all four are bit-for-bit
unchanged afterward, then confirming the canonical owner still authorizes
normally.

This restores exact symmetry with `abort_proposal_release` (N6, same
correction as R5): both mutating entry points now treat a wrong-strategy
caller as inert, never as a risk event against the proposal it names.
Canonical ownership itself is unaffected by this correction -- it is still
established exactly once, only by `commit_proposal_decision` (N5), and
this fix only changes what `authorize_proposal_release` does once it
reads that ownership and finds it does not match the caller.

**Scope, stated explicitly.** This is an API-level identity boundary, not
a claim of process- or memory-level security isolation: any code linked
into the same binary that can call `RiskEngine`'s public API at all can
call it with any `strategy_id` string it likes -- the guarantee is that
doing so under the WRONG one can never mutate or disclose another
strategy's proposal, not that identities are cryptographically
authenticated. `docs/LIMITATIONS.md` is unchanged by this correction; no
new limitation is introduced, and none of R2/R4/R6/R8/R9/R11/N1/N2/N3/N5/
N6/N7/N8 is touched.

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
- `tests/cpp/unit/test_risk_engine_reservation_repair.cpp`'s
  `ProposalAtomicSeamRevalidation` suite (Correction 2): the reviewer's
  exact exposure-reduction attack rejects atomically at commit, not just
  eventually at the seam; an out-of-band change that makes only ONE leg of
  a two-leg proposal unsafe rejects the WHOLE proposal, even when the
  unaffected leg's own order is submitted first; the same safe proposal
  with no late change still approves both legs (no spurious rejection); a
  look-alike proposal cannot borrow another proposal's cached seam
  verdict; a different cumulative control (portfolio notional) is proven
  immune to the same prefix-denominator defect the concentration case
  exposed; a flat-book first position under a sub-1.0 concentration limit
  is proven to reject honestly, by definition, not by bug.
- `tests/cpp/unit/test_calendar_spread_risk_exchange_integration.cpp`'s
  `ConcentrationBreachRejectsBothLegsAtomicallyInTheRealSeamNeverJustOne`:
  the same proposal-atomicity property through the real composition root,
  a real `RiskEngineGate`/`OrderManager` and a real exchange -- an unsafe
  two-leg proposal is rejected before either leg is armed, reserved, or
  submitted.

## Owner approval

Implied by merged `m5-architecture-transition`/`m5-participant-app-integration`
(PR #13's activation policy); this ADR is filed alongside the implementation
it documents.
