#pragma once

#include "cpp/common/clock.hpp"
#include "cpp/participant/oms/risk_gate.hpp"
#include "cpp/participant/risk/risk_engine.hpp"

/// The `oms::RiskGate` adapter (AEGIS-120; ADR-0027).
///
/// `cpp-participant-risk` cannot implement `oms::RiskGate` itself:
/// `cpp-participant-oms` already depends on `cpp-participant-risk`
/// (`configs/architecture_rules.yaml`), so the reverse edge would be a
/// cycle. `cpp-participant-app` -- the only layer permitted to see both --
/// is therefore the only place this adapter can legally live. It translates
/// the OMS's `NewOrderCommand` into a call to `risk::RiskEngine::decide_order`,
/// which is where `commit_proposal_decision`'s pending-leg bookkeeping is
/// actually consumed; an order this gate is asked to decide with no matching
/// armed leg is rejected with `kUnexpectedOrder` (`RiskEngine::decide_order`),
/// the structural defence against a caller that tried to reach the OMS
/// without going through the proposal-level decision first.
namespace aegis::participant::app {

class RiskEngineGate final : public oms::RiskGate {
 public:
  RiskEngineGate(risk::RiskEngine& engine, common::WallClock& clock)
      : engine_(&engine), clock_(&clock) {}

  [[nodiscard]] oms::RiskDecision decide(
      const events::exchange::NewOrderCommand& command) const override;

 private:
  risk::RiskEngine* engine_;
  common::WallClock* clock_;
};

}  // namespace aegis::participant::app
