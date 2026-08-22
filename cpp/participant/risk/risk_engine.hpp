#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/participant/risk/risk_audit.hpp"
#include "cpp/participant/risk/risk_limits.hpp"
#include "cpp/participant/risk/risk_state.hpp"
#include "cpp/participant/risk/risk_types.hpp"
#include "cpp/statistics/drawdown_tracker.hpp"
#include "cpp/statistics/realized_volatility.hpp"

/// The M5 risk engine (AEGIS-120..138; ADR-0027, ADR-0028).
///
/// Two entry points with different contracts, matching the mandatory seam's
/// own shape (`RiskGate::evaluate` is `const`, `RiskGate::decide` mutates):
///   * `evaluate`/`evaluate_proposal` are pure preflight -- they read
///     `state_` but never mutate it, so a caller can ask "would this be
///     allowed" any number of times with no side effect.
///   * `commit_proposal_decision`/`decide_order` are the enforcing path.
///
/// # Reservation timing and exact identity (M5 closure repair)
///
/// An independent risk-safety review found the ORIGINAL split -- reserve
/// exposure at `decide_order`, match a pending leg by
/// `(instrument_id, side, quantity_units)` -- unsafe: two proposals
/// individually within a limit could jointly breach it (nothing counted the
/// first proposal's exposure until its order physically reached the seam),
/// and an order could match a look-alike ARMED LEG from a different
/// strategy/proposal purely by coincidence of economics. Both are fixed by
/// changing WHEN and BY WHAT KEY a leg's exposure is tracked, not by adding
/// checks:
///
///   * `commit_proposal_decision` reserves ALL of an approved/resized
///     proposal's legs' exposure IMMEDIATELY -- before either leg reaches
///     the OMS -- keyed by `PendingLegKey{strategy_id, proposal_id,
///     leg_index}`, since no `client_order_id` exists yet. Every cumulative
///     control already reads `RiskState::reserved_units`/
///     `all_reservations`, so a second proposal committed before the
///     first's orders reach the seam now correctly sees the first's
///     reservation. This also makes replay/duplicate handling exact:
///     `proposal_id` (already globally unique by convention, ADR-0027) that
///     already has a terminal decision returns that SAME decision rather
///     than re-deciding, so `RiskAuditLog::proposal_decision_count` can
///     never exceed 1 and a replay can never re-arm or re-reserve.
///   * The composition root STAGES each leg's future `client_order_id`
///     (`OrderManager::next_client_order_id()`, whose ids are consecutive,
///     so all N are known before the first `submit_new_order`) against its
///     `PendingLegKey` via `stage_proposal_release`, BEFORE any order
///     reaches the OMS. `decide_order` resolves a `client_order_id` to its
///     EXACT `PendingLegKey` through that staging -- never by searching
///     economics -- and only then TRANSITIONS the existing leg reservation
///     to be `client_order_id`-keyed for the ordinary fill/release
///     lifecycle. It never reserves a second time.
///
/// An order that reaches `decide_order` with no staged identity is rejected
/// with `kUnexpectedOrder` -- the structural defence against a caller that
/// tried to skip the proposal-level decision.
///
/// # The proposal release epoch (M5 closure repair, ADR-0027 "Correction 3")
///
/// Two earlier repairs moved toward proposal atomicity and each fell short
/// in a way an independent risk-safety review reproduced:
///
///   * `evaluate_proposal` originally judged each leg against a PREFIX
///     overlay, so a later leg REDUCING exposure was invisible to an
///     earlier leg's cumulative check. Fixed (and still fixed here): Phase
///     A resolves every leg's non-cumulative admission and builds ONE final
///     combined `EvaluationOverlay` from every leg's resolved quantity;
///     Phase B judges each leg's cumulative controls
///     (`check_cumulative_controls`) against that SAME final overlay with
///     only that leg's own contribution excluded first, so it counts
///     exactly once.
///   * The seam then cached a whole-proposal revalidation -- but the epoch
///     began when the FIRST leg reached `decide_order`, i.e. at a moment
///     that leg was already being released. That bought atomicity by
///     giving up freshness: every LATER leg re-checked nothing, so a kill
///     switch, a disconnect, or an exposure breach arriving between two
///     legs did not stop the second one (contradicting AEGIS-135's
///     "prevents new orders"). And `kIdentityMismatch` rejected and
///     released only the offending leg, leaving a correct sibling free to
///     execute alone.
///
/// The naive repair -- cache the cumulative verdict but re-run hard safety
/// per leg -- is deliberately NOT what this engine does, because it merely
/// relocates the hazard: leg 0 released, state changes, leg 1 rejects,
/// naked leg. Instead the epoch moves EARLIER, to a point where nothing has
/// been released yet:
///
///   1. `commit_proposal_decision` reserves and arms every leg (above).
///   2. The composition root builds all N constituent commands and calls
///      `stage_proposal_release` with every one of them. Staging is not
///      permission; the proposal sits in `kStaging` and no constituent is
///      executable.
///   3. `authorize_proposal_release` performs ONE fresh whole-proposal
///      authorization while ZERO legs have been released: every committed
///      leg must have a staged constituent whose economics match it, and
///      every leg must pass `revalidate_at_seam` against CURRENT state.
///      Either ALL constituents become authorized (`kAuthorizedForRelease`)
///      or NONE do (`kRejectedAtRelease`, every reservation released, every
///      leg invalidated, permanently).
///   4. Only then may individual `decide_order` calls CONSUME that
///      authorization. They perform per-order consumption checks only --
///      exact `PendingLegKey`, exact immutable economics, not-already-
///      consumed -- and compute no second proposal-level safety verdict.
///
/// Step 4 deliberately does not re-run kill switches, connectivity,
/// staleness or any cumulative control: those were checked at step 3, when
/// rejecting was still free. Re-running them here is precisely what would
/// let leg 1 contradict leg 0.
///
/// # What this does and does not guarantee
///
/// RISK-DECISION atomicity: before any constituent of a proposal is
/// released, AEGIS makes one all-or-none authorization, and afterwards
/// `decide_order` computes no further proposal-level safety verdict --
/// no halt, connectivity, staleness or cumulative-control question is asked
/// a second time (M5 closure repair, R4: this is the corrected, narrower
/// statement of the claim; the previous "never produces a contradictory
/// per-leg risk verdict" wording was an overclaim). One integrity backstop
/// remains: if an order actually submitted to `decide_order` disagrees on
/// instrument/side/quantity with the exact economics that constituent was
/// staged and authorized under, THAT ONE ORDER is rejected
/// `kIdentityMismatch` while its siblings are unaffected. This is not a
/// second risk judgment -- it consults no mutable safety state, so it
/// cannot recreate the naked-leg hazard the release epoch exists to
/// prevent -- it only refuses to execute something other than what was
/// actually authorized. In the real composition root
/// (`cpp/participant/app/participant_run.cpp`), the staged and submitted
/// economics are read from the same `StrategyLeg` fields, so this backstop
/// is verified NOT to fire on that path today; it exists for any caller
/// that drives this seam directly and could otherwise submit something
/// other than what it staged.
///
/// NOT exchange/broker execution atomicity: after authorization, the
/// transport or the exchange can still accept one leg and fail another --
/// AEGIS has no basket/atomic multi-leg execution primitive. A kill switch
/// tripping after authorization blocks all SUBSEQUENT proposals and drives
/// the existing emergency-cancel path over live orders; it does not
/// retroactively split an authorization already granted. That residual is
/// execution risk, not a contradictory risk verdict, and is stated in
/// `docs/LIMITATIONS.md` rather than implied away.
///
/// Cumulative limits are evaluated against the proposal's FINAL NET
/// PROJECTED state, not against every sequential intermediate state its
/// legs could pass through during non-atomic execution. See
/// `docs/LIMITATIONS.md`.
namespace aegis::participant::risk {

/// The seam-facing verdict `decide_order` returns; `app::RiskEngineGate`
/// translates this to `oms::RiskDecision`. Kept free of any OMS type so this
/// header never has to include `cpp/participant/oms/risk_gate.hpp`.
struct OmsDecision {
  RiskVerdict verdict{RiskVerdict::kReject};
  std::int64_t approved_quantity_units{0};
  ReasonCode reason_code{ReasonCode::kNone};
  std::string reason;
};

class RiskEngine {
 public:
  explicit RiskEngine(RiskLimitsConfig config) : config_(std::move(config)) {}

  // ---- pure preflight ---------------------------------------------------
  [[nodiscard]] LegDecision evaluate(const OrderRequest& request) const;
  [[nodiscard]] ProposalDecisionResult evaluate_proposal(
      const std::string& strategy_id, const std::string& proposal_id,
      const std::vector<OrderRequest>& legs) const;

  // ---- enforcing ----------------------------------------------------
  const ProposalRiskDecision& commit_proposal_decision(const std::string& strategy_id,
                                                       const std::string& proposal_id,
                                                       const std::vector<OrderRequest>& legs,
                                                       common::Nanos decided_at_nanos);

  [[nodiscard]] OmsDecision decide_order(std::uint32_t instrument_id, Side side,
                                         std::int64_t quantity_units, std::uint64_t client_order_id,
                                         common::Nanos now_nanos);

  /// Stages the constituent orders the composition root is about to submit
  /// for an already-committed proposal, BEFORE any of them is released
  /// (ADR-0027 "Correction 3"). Each entry binds a `client_order_id` the OMS
  /// is about to assign to the exact armed leg it belongs to, plus the
  /// economics that order will carry. Staging is not permission: it moves
  /// the proposal to `kStaging`, and only `authorize_proposal_release` can
  /// make any constituent executable. May be called once with every
  /// constituent, or incrementally; either way authorization requires the
  /// staged set to cover every committed leg exactly.
  ///
  /// A `proposal_id` that `commit_proposal_decision` has not genuinely
  /// committed yet is a NO-OP (M5 closure repair, N5): no map entry is
  /// created, so a pre-commit staging call can never establish canonical
  /// ownership of a `proposal_id`, nor leave any state a later legitimate
  /// commit would have to overwrite or could be poisoned by.
  ///
  /// CANONICAL ATTRIBUTION IS IMMUTABLE (M5 closure repair, R5/NB-2
  /// corrected): once a proposal is committed, its `strategy_id` is fixed
  /// by `commit_proposal_decision` alone, and this call NEVER overwrites
  /// it. A call naming a disagreeing `strategy_id` against an EXISTING
  /// committed proposal is UNAUTHORIZED API MISUSE, not evidence that the
  /// named proposal itself is risky or malformed -- it is fully inert: it
  /// returns immediately, before touching `record.state` or anything else,
  /// exactly like `authorize_proposal_release` (N4) and
  /// `abort_proposal_release` (N6). (An earlier version of this check
  /// latched a persistent `attribution_mismatch` flag on a mismatch and
  /// moved the record to `kStaging` -- silently sabotaging the canonical
  /// owner's OWN future `authorize_proposal_release` call, which discovered
  /// the flag and rejected the whole proposal, misattributing that
  /// rejection to the victim's own `strategy_id` in the audit trail. Fixed.)
  void stage_proposal_release(const std::string& strategy_id, const std::string& proposal_id,
                              const std::vector<StagedOrderIdentity>& staged);

  /// THE PROPOSAL RELEASE EPOCH: one fresh, whole-proposal risk
  /// authorization performed while ZERO of the proposal's constituents have
  /// been released. Verifies that every committed leg has a staged
  /// constituent whose economics match it, then revalidates every leg
  /// against CURRENT mutable state (halts, connectivity, market
  /// staleness/collar, the volatility hard-reject gate, and every
  /// cumulative control over the final combined overlay, each leg's own
  /// reservation counted exactly once). Either ALL constituents become
  /// authorized, or NONE do and every reservation the proposal held is
  /// released.
  ///
  /// IDENTITY IS CHECKED BEFORE ANY LIFECYCLE-STATE DISCLOSURE (M5 closure
  /// repair, R4/N4, corrected): once a `proposal_id` is known to be
  /// genuinely committed, `strategy_id` must equal its canonical committed
  /// strategy identity, and this is verified before this call inspects,
  /// mutates or discloses ANYTHING about that proposal's actual lifecycle
  /// state. A wrong-strategy call against an EXISTING committed proposal is
  /// an UNAUTHORIZED QUERY, not a risk rejection of the proposal it names,
  /// and it is fully inert: it never mutates release state, reservations,
  /// pending legs, staged identities or the audit trail, and it never
  /// appends a release-lifecycle event -- it returns a single generic
  /// `kIdentityMismatch` denial, identical regardless of whether that
  /// EXISTING proposal is staged, authorized, rejected, aborted or
  /// completed, and never reveals the real stored lifecycle decision.
  /// (An earlier version of this check ran AFTER the terminal-state lookup
  /// and routed a mismatch through `reject_proposal_release` -- a caller
  /// with no relationship to a proposal could destroy it and reclaim its
  /// reserved risk budget, and a wrong-strategy query of an already-terminal
  /// proposal received that proposal's REAL stored decision. Both were the
  /// exact hazard this repair closes.)
  ///
  /// NOT CLAIMED (M5 closure repair, NB-1 correction): an UNKNOWN
  /// `proposal_id` (one `commit_proposal_decision` never genuinely
  /// committed) is handled by a SEPARATE, EARLIER check and returns
  /// `kUnexpectedOrder`, not the generic `kIdentityMismatch` denial -- so a
  /// caller CAN distinguish "this proposal_id was never committed" from
  /// "this proposal_id belongs to a different strategy" by reason code
  /// alone. AEGIS does not claim proposal-ID confidentiality, and this is
  /// not an OS- or process-level security isolation boundary between
  /// arbitrary in-process callers: it is an API-level identity boundary
  /// matching `abort_proposal_release`'s own (below), whose guarantee is
  /// narrower and precise -- a wrong-strategy caller can never MUTATE
  /// another strategy's proposal, nor read its REAL stored lifecycle
  /// decision, but existence of a `proposal_id` is not hidden.
  ///
  /// Idempotent and terminal for the CANONICAL owner (M5 closure repair,
  /// N1): once the proposal's release lifecycle reaches
  /// `kAuthorizedForRelease`, `kRejectedAtRelease`, `kAborted` or
  /// `kCompleted`, every later call FROM THE OWNING STRATEGY returns that
  /// same decision and records no new audit event -- `kAborted` was the
  /// terminal state missing here before this repair, letting a call AFTER
  /// a deliberate `abort_proposal_release` fall through and overwrite the
  /// abort with a spurious rejection. See `docs/BUILD_STATE.md`/ADR-0027
  /// for why "at most one terminal decision" is tracked PER LIFECYCLE
  /// TRANSITION KIND (authorize / reject-at-release / abort), not as a
  /// single global "never more than one release record ever" count -- an
  /// authorize followed by a genuine later abort is two real, distinct,
  /// truthfully audited events, and AEGIS-137's actual "exactly one"
  /// invariant belongs to `ProposalRiskDecision`/`proposal_decision_count`,
  /// never to this. After `kAuthorizedForRelease`, `decide_order` performs
  /// NO further proposal-level safety evaluation -- see the class docs for
  /// why re-checking per leg would recreate the naked-leg hazard this
  /// exists to prevent.
  ProposalReleaseDecision authorize_proposal_release(const std::string& strategy_id,
                                                     const std::string& proposal_id,
                                                     common::Nanos now_nanos);

  /// The proposal's current release state (`kCommitted` if it has none).
  [[nodiscard]] ProposalReleaseState proposal_release_state(const std::string& proposal_id) const;

  /// Deliberately abandons an authorized proposal's still-unconsumed
  /// constituents (M5 closure repair, R3): a caller that authorized a
  /// proposal and then decides -- for a reason outside RiskEngine's own
  /// visibility -- not to submit some or all of its legs would otherwise
  /// strand their reservations forever, since nothing else ever calls
  /// `decide_order` for them. Releases the reservation of every leg of
  /// `proposal_id` still present in the pending set (i.e. not yet consumed
  /// by `decide_order`) and moves the proposal to the terminal `kAborted`
  /// state. Does NOT touch a leg that already consumed its authorization --
  /// that leg is live (or terminal) through the ordinary OMS/fill lifecycle,
  /// and aborting the proposal never rolls back an order already submitted.
  ///
  /// Requires `strategy_id` to match the proposal's canonical committed
  /// strategy identity (M5 closure repair, N6): a caller cannot abort a
  /// DIFFERENT strategy's proposal merely by knowing its `proposal_id` --
  /// a mismatch is a pure no-op, mutating nothing and auditing nothing. A
  /// `proposal_id` `commit_proposal_decision` has not genuinely committed
  /// yet is also a no-op that creates no persistent state, so a pre-emptive
  /// abort can never poison a later legitimate commit of that id.
  ///
  /// Idempotent and terminal (M5 closure repair, N1): a call against a
  /// proposal that is already `kAborted`, `kRejectedAtRelease` or
  /// `kCompleted` is a no-op that returns the existing decision unchanged
  /// and records no second audit entry. See `authorize_proposal_release`'s
  /// own doc for why "at most one release record" is tracked per lifecycle
  /// transition kind, not as a single count across authorize+abort.
  ProposalReleaseDecision abort_proposal_release(const std::string& strategy_id,
                                                 const std::string& proposal_id, std::string reason,
                                                 common::Nanos now_nanos);

  /// AEGIS-128's cancel half. `bypass_safety = true` (a kill-switch or
  /// connectivity-loss cancel) is never subject to the rate limit and never
  /// consumes a token from it -- the critical safety rule this engine must
  /// not violate is a client message-rate budget blocking the system's own
  /// ability to flatten a position. An ordinary client-initiated cancel
  /// (`bypass_safety = false`) is throttled exactly like an order.
  [[nodiscard]] bool allow_cancel(common::Nanos now_nanos, bool bypass_safety = false);

  // ---- fill / lifecycle feedback (fed by the composition root) ---------
  void on_fill(std::uint64_t client_order_id, std::uint32_t instrument_id, Side side,
               std::int64_t fill_quantity_units);
  void on_order_terminated(std::uint64_t client_order_id);
  void on_order_rejected(std::uint64_t client_order_id);
  /// General-purpose release, for any terminal path a caller with its own
  /// visibility needs to signal explicitly (manual reconciliation, an
  /// out-of-band confirmation) -- NOT the normal submission-failure path,
  /// which `app::RiskReleasingExecutionAdapter` handles automatically by
  /// wrapping the concrete `ExecutionAdapter` the OMS seam cannot itself be
  /// modified to observe (`cpp/participant/oms/**` is unmodified by M5).
  /// Exercised directly by `tests/cpp/unit/test_risk_engine.cpp`.
  void release_reservation(std::uint64_t client_order_id) {
    state_.release_reservation(client_order_id);
  }

  // ---- market feedback ---------------------------------------------------
  void on_market_data(std::uint32_t instrument_id, std::int64_t reference_price_units,
                      common::Nanos observed_at_nanos, bool valid);

  // ---- portfolio feedback (equity/session P&L, fed by the composition root
  // reading Portfolio -- risk never depends on portfolio::Portfolio itself)
  void on_equity_update(std::int64_t equity_units);
  void on_session_pnl_update(std::int64_t cumulative_session_pnl_units);
  void start_new_session() { state_.start_new_session(); }

  // ---- connectivity -------------------------------------------------
  void on_feed_disconnected() { state_.set_feed_connected(false); }
  void on_feed_reconnected() { state_.set_feed_connected(true); }
  void on_exchange_disconnected() { state_.set_exchange_connected(false); }
  void on_exchange_reconnected() { state_.set_exchange_connected(true); }
  void on_broker_disconnected() { state_.set_broker_connected(false); }
  void on_broker_reconnected() { state_.set_broker_connected(true); }

  // ---- kill switches (idempotent: returns true only on the first trip) --
  bool trip_strategy(const std::string& strategy_id) { return state_.trip_strategy(strategy_id); }
  bool trip_global() { return state_.trip_global(); }
  [[nodiscard]] bool is_strategy_halted(const std::string& strategy_id) const {
    return state_.is_strategy_halted(strategy_id);
  }
  [[nodiscard]] bool is_globally_halted() const { return state_.is_globally_halted(); }

  [[nodiscard]] const RiskAuditLog& audit_log() const { return audit_log_; }
  [[nodiscard]] const RiskState& state() const { return state_; }
  [[nodiscard]] const RiskLimitsConfig& config() const { return config_; }

 private:
  /// Extra exposure/notional/rate-limit consumption from legs of the same
  /// proposal already folded in, so `evaluate_proposal` sees the combined
  /// effect of both legs rather than checking each in isolation
  /// (AEGIS-137/ADR-0027's atomicity: a spread that individually passes leg
  /// by leg but jointly breaches a portfolio-level limit must still reject
  /// both legs).
  struct EvaluationOverlay {
    std::unordered_map<std::uint32_t, std::int64_t> extra_reserved_by_instrument;
    std::uint32_t extra_orders_in_window{0};
  };

  [[nodiscard]] LegDecision evaluate_leg(const OrderRequest& request,
                                         const EvaluationOverlay& overlay) const;

  // One helper per control group of ADR-0028, run by `evaluate_leg` in a
  // fixed documented order. Each returns an engaged optional to mean "this
  // group rejected", disengaged to mean "nothing here objected". Split out
  // so no single function carries every control's branching at once.
  //
  // Control group 0 (M5 closure repair, R1): quantity is a positive
  // MAGNITUDE, `Side` alone carries direction. Checked first, before any
  // other control reads `request.quantity_units` at all -- a malformed
  // non-positive quantity must never reach a sign-sensitive computation
  // (position projection, notional, reservation) as if it were legitimate
  // input.
  [[nodiscard]] static std::optional<LegDecision> check_quantity_validity(
      const OrderRequest& request);
  [[nodiscard]] std::optional<LegDecision> check_halts_and_connectivity(
      const OrderRequest& request) const;
  [[nodiscard]] std::optional<LegDecision> check_market_state(const OrderRequest& request,
                                                              const MarketQuote*& quote_out) const;
  [[nodiscard]] std::optional<LegDecision> check_request_admission(
      const OrderRequest& request, const EvaluationOverlay& overlay) const;
  [[nodiscard]] std::optional<LegDecision> resolve_effective_quantity(
      const OrderRequest& request, std::int64_t& effective_quantity, bool& resized) const;
  /// Defense-in-depth postcondition (M5 closure repair, N2): rejects
  /// `kInvalidLimitConfiguration` if `effective_quantity` is not a positive
  /// magnitude -- the symptom of a malformed configured limit, since
  /// `check_quantity_validity` already guarantees the REQUEST itself was
  /// positive. Called after every `resolve_effective_quantity`, before the
  /// quantity is used for reservation or returned as an executable verdict.
  [[nodiscard]] static std::optional<LegDecision> check_approved_quantity_postcondition(
      std::int64_t effective_quantity);
  [[nodiscard]] std::optional<LegDecision> check_position_and_notional(
      const OrderRequest& request, const EvaluationOverlay& overlay,
      std::int64_t effective_quantity, EvaluationOverlay& portfolio_overlay_out,
      std::int64_t& portfolio_notional_out) const;
  [[nodiscard]] std::optional<LegDecision> check_group_exposure(
      const OrderRequest& request, const EvaluationOverlay& portfolio_overlay) const;
  [[nodiscard]] std::optional<LegDecision> check_one_exposure_group(
      const std::string& group_key, const std::unordered_map<std::string, std::int64_t>& limits,
      const std::function<const std::string&(const InstrumentInfo&)>& key_of,
      const EvaluationOverlay& portfolio_overlay, ReasonCode reason_code,
      const std::string& reason) const;
  [[nodiscard]] std::optional<LegDecision> check_concentration(
      const OrderRequest& request, const EvaluationOverlay& portfolio_overlay,
      std::int64_t portfolio_notional) const;
  [[nodiscard]] std::optional<LegDecision> check_margin_and_leverage(
      const EvaluationOverlay& portfolio_overlay, std::int64_t portfolio_notional) const;
  /// Control groups 8-12 (position, notional, market/sector, concentration,
  /// margin/leverage) as one unit: the shared cumulative-control pipeline
  /// `evaluate_leg`, `evaluate_proposal`'s Phase B, and `revalidate_at_seam`
  /// all call, so there is exactly one place this sequence is written, not
  /// three drifting copies. `context_overlay` is whatever the caller has
  /// already established as "everything except this leg's own candidate,
  /// counted once" -- an empty overlay for a lone `evaluate()` call, the
  /// proposal's final combined overlay with this leg's own share excluded
  /// for `evaluate_proposal`, or the seam's own-reservation-excluded overlay
  /// for `revalidate_at_seam`.
  [[nodiscard]] std::optional<LegDecision> check_cumulative_controls(
      const OrderRequest& request, std::int64_t effective_quantity,
      const EvaluationOverlay& context_overlay) const;
  /// The volatility HARD-REJECT safety gate only (M5 closure repair, R6) --
  /// realized volatility at or beyond `hard_reject_multiple * target_volatility`
  /// for `instrument_id`. Deliberately excludes the RESIZE branch of
  /// `resolve_effective_quantity`: sizing is decided once, at commit, and
  /// stays immutable through release (re-opening it would mean the quantity
  /// a proposal was staged and authorized under could silently change).
  /// The hard-reject branch is different in kind -- it is a safety gate, not
  /// a sizing decision -- so `revalidate_at_seam` re-runs THIS check fresh
  /// against current volatility, exactly like every other hard safety
  /// control. A hard volatility safety breach discovered here rejects the
  /// WHOLE proposal at release, before anything is sent.
  [[nodiscard]] std::optional<LegDecision> check_volatility_hard_reject(
      std::uint32_t instrument_id) const;
  [[nodiscard]] std::int64_t notional_units_base_currency(std::uint32_t instrument_id,
                                                          std::int64_t quantity_units,
                                                          std::int64_t price_units,
                                                          bool& unsupported_currency) const;
  [[nodiscard]] std::int64_t gross_portfolio_notional_units(const EvaluationOverlay& overlay) const;
  [[nodiscard]] std::int64_t group_gross_notional_units(const std::vector<std::uint32_t>& members,
                                                        const EvaluationOverlay& overlay) const;
  [[nodiscard]] double realized_volatility(std::uint32_t instrument_id) const;

  struct PendingLeg {
    std::string strategy_id;
    std::string proposal_id;
    std::uint32_t leg_index{0};
    std::uint32_t instrument_id{0};
    Side side{Side::kBuy};
    std::int64_t price_units{0};  ///< The leg's own limit/reference price, for seam revalidation.
    std::int64_t requested_quantity_units{0};
    std::int64_t approved_quantity_units{0};
    RiskVerdict verdict{RiskVerdict::kReject};
    ReasonCode reason_code{ReasonCode::kNone};
    std::string reason;
  };

  /// Re-checks the mutable safety state a proposal must not trust forever
  /// (halts, connectivity, market staleness/collar, the volatility
  /// HARD-REJECT gate, and every cumulative exposure/margin/leverage
  /// control) against CURRENT `state_`, excluding `pending`'s own
  /// already-reserved contribution so it is counted exactly once, not
  /// twice. Deliberately does NOT re-run the order-quantity-cap or the
  /// volatility-RESIZE branch (sizing is immutable once approved) or
  /// idempotency/rate-limit admission (already consumed at commit time;
  /// re-running either would reject every order, since the dedupe key is
  /// already marked seen and the rate-limit event already recorded). It DOES
  /// re-run the volatility HARD-REJECT branch (M5 closure repair, R6): that
  /// branch is a safety gate, not a sizing choice, and every other hard
  /// safety gate here is already re-checked fresh -- a proposal safely sized
  /// while volatility was calm but whose reference instrument has since
  /// spiked past the hard-reject multiple must not be released just because
  /// sizing itself stays frozen. Called ONLY from `authorize_proposal_release`,
  /// over every leg of one proposal, at a moment when none of them has been
  /// released -- never against a single leg of a multi-leg proposal in
  /// isolation, and never after a sibling is already executable.
  [[nodiscard]] std::optional<LegDecision> revalidate_at_seam(const PendingLeg& pending,
                                                              common::Nanos now_nanos) const;

  /// Everything the engine tracks for one committed proposal between
  /// `commit_proposal_decision` and the last constituent consuming its
  /// authorization.
  struct ProposalReleaseRecord {
    ProposalReleaseState state{ProposalReleaseState::kCommitted};
    ReasonCode reason_code{ReasonCode::kNone};
    std::string reason;
    /// The proposal's ONE canonical strategy identity (M5 closure repair,
    /// R5/N5): set EXACTLY ONCE, by `commit_proposal_decision`, and by
    /// nothing else. `stage_proposal_release`/`authorize_proposal_release`/
    /// `abort_proposal_release` all require `committed == true` before they
    /// touch a record at all (below), so none of them can ever be the FIRST
    /// call to establish this value -- a pre-commit call on an unknown
    /// `proposal_id` creates no map entry and therefore cannot "win the
    /// race" to own it. Every audit record this proposal ever produces
    /// (`ProposalRiskDecision`, `ProposalReleaseRiskDecision`) attributes to
    /// this value.
    std::string strategy_id;
    /// True only once `commit_proposal_decision` has genuinely committed
    /// this `proposal_id` (approved or resized, with real `pending_legs_`
    /// entries) -- never set by `stage_proposal_release`,
    /// `authorize_proposal_release` or `abort_proposal_release` (M5 closure
    /// repair, N5/N6). This is what lets those three calls tell "a proposal
    /// that was genuinely committed" apart from "a `proposal_id` nobody has
    /// committed yet, whose map entry (if any) is a stray, unauthoritative
    /// artifact" -- so none of them can establish persistent ownership or
    /// poison a `proposal_id` before its real commit happens.
    bool committed{false};
    /// leg_index -> the constituent order staged for it.
    std::unordered_map<std::uint32_t, StagedOrderIdentity> staged_by_leg_index;
    /// How many authorized constituents have not yet consumed the
    /// authorization; reaching zero moves the proposal to `kCompleted`.
    std::size_t outstanding_authorized_legs{0};
  };

  /// Rejects `record`'s whole proposal: releases every leg reservation it
  /// still holds, drops every armed leg and staged identity, and records the
  /// one terminal release decision. Used by every `authorize_proposal_release`
  /// failure path so "reject" always means the same thing.
  ProposalReleaseDecision reject_proposal_release(const std::string& proposal_id,
                                                  ProposalReleaseRecord& record,
                                                  ReasonCode reason_code, std::string reason,
                                                  common::Nanos now_nanos);

  RiskLimitsConfig config_;
  RiskState state_;
  RiskAuditLog audit_log_;
  std::unordered_map<std::uint32_t, stats::RollingRealizedVolatility> volatility_by_instrument_;
  std::unordered_map<std::uint32_t, std::int64_t> last_price_by_instrument_;
  stats::DrawdownTracker drawdown_;
  std::unordered_map<PendingLegKey, PendingLeg, PendingLegKeyHash> pending_legs_;
  /// `client_order_id -> PendingLegKey`, populated by
  /// `register_pending_order_identity` and consumed (erased) by
  /// `decide_order` the moment it resolves an order -- never searched by
  /// economics.
  std::unordered_map<std::uint64_t, PendingLegKey> order_identity_by_client_order_id_;
  /// `proposal_id -> ProposalReleaseRecord`, keyed the same way
  /// `RiskAuditLog::find_proposal_decision` already treats `proposal_id`
  /// (globally unique by convention, ADR-0027) -- not a second, narrower
  /// identity notion.
  std::unordered_map<std::string, ProposalReleaseRecord> proposal_release_by_id_;
};

}  // namespace aegis::participant::risk
