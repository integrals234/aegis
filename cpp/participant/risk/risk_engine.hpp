#pragma once

#include <cstdint>
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
///   * `commit_proposal_decision`/`decide_order` are the enforcing path:
///     they record audit entries, consume rate-limit budget, mark
///     idempotency keys and reserve exposure. `commit_proposal_decision` is
///     what makes the whole-proposal check in `evaluate_proposal` atomic in
///     practice -- it is called once, before either leg reaches the OMS, and
///     records the ONE terminal `ProposalRiskDecision` a proposal_id ever
///     gets. `decide_order` is the OMS seam entry point
///     (`app::RiskEngineGate::decide`): it consumes the pending leg
///     `commit_proposal_decision` armed, reserves exposure now that a
///     `client_order_id` exists, and records the subordinate
///     `OrderRiskDecision`. An order that reaches `decide_order` with no
///     matching armed leg is rejected with `kUnexpectedOrder` -- the
///     structural defence against a caller that tried to skip the
///     proposal-level decision.
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
  void release_reservation(std::uint64_t client_order_id) { state_.release_reservation(client_order_id); }

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
    std::int64_t requested_quantity_units{0};
    std::int64_t approved_quantity_units{0};
    RiskVerdict verdict{RiskVerdict::kReject};
    ReasonCode reason_code{ReasonCode::kNone};
    std::string reason;
  };

  RiskLimitsConfig config_;
  RiskState state_;
  RiskAuditLog audit_log_;
  std::unordered_map<std::uint32_t, stats::RollingRealizedVolatility> volatility_by_instrument_;
  std::unordered_map<std::uint32_t, std::int64_t> last_price_by_instrument_;
  stats::DrawdownTracker drawdown_;
  std::vector<PendingLeg> pending_legs_;
};

}  // namespace aegis::participant::risk
