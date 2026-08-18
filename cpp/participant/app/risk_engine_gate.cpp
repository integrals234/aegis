#include "cpp/participant/app/risk_engine_gate.hpp"

namespace aegis::participant::app {

oms::RiskDecision RiskEngineGate::decide(const events::exchange::NewOrderCommand& command) const {
  // `decide` is declared const (oms::RiskGate's contract), but `engine_` is a
  // plain (non-const-pointee) pointer member: `const` here means `this` is
  // const, not `*engine_`, so the mutating call below is legal and honest --
  // the engine is injected and owned by the composition root, exactly like
  // OrderManager's own adapter/gate references (ADR-0027).
  const risk::OmsDecision decision = engine_->decide_order(
      command.instrument_id, command.side, command.quantity_units, command.client_order_id,
      clock_->now_utc());

  oms::RiskVerdict verdict = oms::RiskVerdict::kReject;
  switch (decision.verdict) {
    case risk::RiskVerdict::kApprove: verdict = oms::RiskVerdict::kApprove; break;
    case risk::RiskVerdict::kResize: verdict = oms::RiskVerdict::kResize; break;
    case risk::RiskVerdict::kReject: verdict = oms::RiskVerdict::kReject; break;
  }
  return oms::RiskDecision{.verdict = verdict,
                           .resized_quantity_units = decision.approved_quantity_units,
                           .reason = decision.reason};
}

}  // namespace aegis::participant::app
