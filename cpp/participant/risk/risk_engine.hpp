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
///   * The composition root registers each leg's future `client_order_id`
///     (`OrderManager::next_client_order_id()`, peeked immediately before
///     `submit_new_order`) against its `PendingLegKey` via
///     `register_pending_order_identity`, BEFORE the order reaches the OMS.
///     `decide_order` resolves a `client_order_id` to its EXACT
///     `PendingLegKey` through that registration -- never by searching
///     economics -- then verifies the order's instrument/side/quantity
///     agree with what was actually reserved (an `kIdentityMismatch`
///     reject, not a silent fall-through, if they do not), revalidates
///     mutable safety state that can have changed since commit (halts,
///     connectivity, market staleness/collar, and every cumulative control
///     excluding this leg's own already-counted reservation), and only then
///     TRANSITIONS the existing leg reservation to be `client_order_id`-keyed
///     for the ordinary fill/release lifecycle -- it never reserves a
///     second time.
///
/// An order that reaches `decide_order` with no registered identity is
/// rejected with `kUnexpectedOrder` -- the structural defence against a
/// caller that tried to skip the proposal-level decision.
///
/// # Proposal-atomic final-overlay evaluation and seam revalidation (M5
/// closure repair)
///
/// A risk-safety review found the reservation/identity repair above still
/// permitted a naked leg through a different door. `evaluate_proposal`
/// evaluated each leg against a PREFIX overlay -- only the legs staged
/// *before* it in iteration order -- so a later leg that REDUCES exposure
/// (a closing or rolling trade) was invisible to an earlier leg's own
/// cumulative-control check. A proposal could therefore commit as APPROVE
/// even though its true, fully-combined projection was unsafe. Separately,
/// `decide_order`/`revalidate_at_seam` judged each leg's safety
/// independently at the seam: one leg could fail a late-breaking
/// revalidation while its sibling, evaluated moments later against
/// different (already-mutated) state, passed -- producing exactly the
/// naked single leg ADR-0027's atomicity exists to prevent, even when both
/// legs were individually "correct" against the state each happened to see
/// at the instant it was checked.
///
/// Both are fixed by making evaluation, not just reservation, proposal-wide:
///
///   * `evaluate_proposal` now runs in two phases. Phase A resolves every
///     leg's non-cumulative admission (halts, connectivity, market state,
///     idempotency/rate-limit, order-quantity-cap/volatility sizing) and
///     builds ONE final combined `EvaluationOverlay` from every leg's own
///     resolved quantity -- the complete proposed portfolio, not a
///     leg-by-leg accumulation. Phase B then judges each leg's cumulative
///     controls (`check_cumulative_controls`) against that SAME final
///     overlay, with only that leg's own contribution excluded first (so
///     re-adding it as the candidate counts it exactly once) -- the
///     identical exclude-then-readd technique `revalidate_at_seam` already
///     used. A proposal whose true combined effect is unsafe now rejects
///     atomically at commit, before anything is armed or reserved,
///     regardless of leg order.
///   * `decide_order` no longer calls `revalidate_at_seam` per leg
///     directly. It first calls `ensure_proposal_seam_revalidated`, which
///     runs (once per proposal, cached in `proposal_seam_state_by_id_`) a
///     single revalidation pass over EVERY still-pending leg of that
///     proposal against current mutable state. If any leg would fail, the
///     WHOLE proposal is marked `kRejectedAtSeam` and every one of its
///     reservations is released immediately -- no leg of that proposal can
///     ever reach `kApprove`/`kResize` after that, regardless of which
///     leg's order happens to arrive at the seam first or what state
///     changes afterward. `decide_order` then only applies that one cached
///     proposal-level outcome to the exact `PendingLegKey` it resolved.
///
/// This guarantees risk-DECISION atomicity: risk approves the whole
/// proposal or rejects the whole proposal, never a mix. It does NOT
/// guarantee atomic EXCHANGE execution -- once risk approves both legs,
/// a transport or exchange failure on one leg after submission can still
/// leave the other filled alone, because this system has no basket/atomic
/// multi-leg execution primitive. That boundary is stated, not implied.
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

  /// Binds a `client_order_id` the OMS is ABOUT TO assign (the composition
  /// root peeks it via `OrderManager::next_client_order_id()` before calling
  /// `submit_new_order`) to the exact armed leg `{strategy_id, proposal_id,
  /// leg_index}` names, so `decide_order` can resolve that `client_order_id`
  /// to its reservation without searching by economics. Returns false (and
  /// registers nothing) if no such leg is currently armed -- a caller
  /// error `decide_order` then reports as `kUnexpectedOrder`, never as a
  /// match against some other leg.
  bool register_pending_order_identity(std::uint64_t client_order_id,
                                       const std::string& strategy_id,
                                       const std::string& proposal_id, std::uint32_t leg_index);

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
  [[nodiscard]] std::optional<LegDecision> check_halts_and_connectivity(
      const OrderRequest& request) const;
  [[nodiscard]] std::optional<LegDecision> check_market_state(const OrderRequest& request,
                                                              const MarketQuote*& quote_out) const;
  [[nodiscard]] std::optional<LegDecision> check_request_admission(
      const OrderRequest& request, const EvaluationOverlay& overlay) const;
  [[nodiscard]] std::optional<LegDecision> resolve_effective_quantity(
      const OrderRequest& request, std::int64_t& effective_quantity, bool& resized) const;
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

  /// Re-checks the mutable safety state a single leg must not trust forever
  /// (halts, connectivity, market staleness/collar, and every cumulative
  /// exposure/margin/leverage control) against CURRENT `state_`, excluding
  /// `pending`'s own already-reserved contribution so it is counted exactly
  /// once, not twice. Deliberately does NOT re-run order-quantity-cap/
  /// volatility-resize (sizing is immutable once approved) or idempotency/
  /// rate-limit admission (already consumed at commit time; re-running
  /// either would reject every order, since the dedupe key is already
  /// marked seen and the rate-limit event already recorded). Called ONLY
  /// from `ensure_proposal_seam_revalidated`, once per pending leg of a
  /// proposal being revalidated together -- never applied to just one leg
  /// of a multi-leg proposal in isolation, which is what let one sibling
  /// fail while the other passed (the naked-leg defect this repair closes).
  [[nodiscard]] std::optional<LegDecision> revalidate_at_seam(const PendingLeg& pending,
                                                              common::Nanos now_nanos) const;

  /// One committed proposal's seam-revalidation epoch: computed AT MOST
  /// ONCE (by `ensure_proposal_seam_revalidated`, the first time any of its
  /// legs reaches `decide_order`) and then trusted by every subsequent leg
  /// of the same proposal, so risk can only ever approve the whole proposal
  /// or reject the whole proposal at the seam -- never a mix.
  enum class ProposalSeamState : std::uint8_t {
    kNotRevalidated = 0,
    kApprovedForRelease = 1,
    kRejectedAtSeam = 2,
  };
  struct ProposalSeamRecord {
    ProposalSeamState state{ProposalSeamState::kNotRevalidated};
    ReasonCode reject_reason_code{ReasonCode::kNone};
    std::string reject_reason;
  };

  /// Idempotent: a no-op if `proposal_id`'s seam epoch was already decided.
  /// Otherwise revalidates every leg of `proposal_id` still in
  /// `pending_legs_` (via `revalidate_at_seam`) and records ONE outcome for
  /// the whole proposal. On the first leg that would fail, marks the
  /// proposal `kRejectedAtSeam` and releases EVERY one of its remaining
  /// leg reservations immediately -- not deferred until each leg's own
  /// `decide_order` call, since by then the proposal's fate is already
  /// known and holding reserved capacity for legs that will never execute
  /// serves no one. If every leg passes, marks it `kApprovedForRelease`;
  /// `decide_order` then still verifies each leg's own identity/economics
  /// and transitions its reservation individually, but never re-runs the
  /// cumulative controls this already decided.
  void ensure_proposal_seam_revalidated(const std::string& proposal_id, common::Nanos now_nanos);

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
  /// `proposal_id -> ProposalSeamRecord`, keyed the same way
  /// `RiskAuditLog::find_proposal_decision` already treats `proposal_id`
  /// (globally unique by convention, ADR-0027) -- not a second, narrower
  /// identity notion.
  std::unordered_map<std::string, ProposalSeamRecord> proposal_seam_state_by_id_;
};

}  // namespace aegis::participant::risk
