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

  /// Re-checks the mutable safety state `decide_order` must not trust
  /// forever (halts, connectivity, market staleness/collar, and every
  /// cumulative exposure/margin/leverage control) against CURRENT `state_`,
  /// excluding `pending`'s own already-reserved contribution so it is
  /// counted exactly once, not twice. Deliberately does NOT re-run
  /// order-quantity-cap/volatility-resize (sizing is immutable once
  /// approved) or idempotency/rate-limit admission (already consumed at
  /// commit time; re-running either would reject every order, since the
  /// dedupe key is already marked seen and the rate-limit event already
  /// recorded) -- see risk_engine.cpp's `revalidate_at_seam` for the exact
  /// reasoning per control.
  [[nodiscard]] std::optional<LegDecision> revalidate_at_seam(const PendingLeg& pending,
                                                              common::Nanos now_nanos) const;

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
};

}  // namespace aegis::participant::risk
