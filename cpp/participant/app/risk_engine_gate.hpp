#pragma once

#include "cpp/common/clock.hpp"
#include "cpp/participant/oms/execution_adapter.hpp"
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

/// Automatic reservation release on submission failure (carry-in fix from
/// Batch 1, ADR-0027 addendum). `OrderManager::submit_new_order`
/// (`cpp/participant/oms`, unmodified -- out of M5's approved scope)
/// discards `ExecutionAdapter::submit`'s boolean return value, so nothing in
/// the OMS seam itself tells `RiskEngine` a send failed. That gap cannot be
/// closed inside the OMS; it can be closed at the ADAPTER, which the
/// composition root is free to choose and which already sees the same
/// `NewOrderCommand` (and therefore the same `client_order_id`) `decide`
/// approved moments earlier.
///
/// This decorator wraps a real `ExecutionAdapter` and, only when `submit`
/// returns `false`, releases the reservation `RiskEngine::decide_order`
/// created for that `client_order_id` -- automatically, with no manual
/// cleanup call required by the caller. `cancel`/`modify` are forwarded
/// unmodified: a cancel or modify failing does not create a NEW reservation
/// to release, and `OrderManager` already tracks their own outcomes through
/// its ordinary lifecycle. `RiskEngine::release_reservation` remains public
/// for its own legitimate, separate use (manual reconciliation, a caller
/// with visibility this adapter does not have) -- it is no longer the
/// *normal* submission-failure path once this decorator is in use.
class RiskReleasingExecutionAdapter final : public oms::ExecutionAdapter {
 public:
  RiskReleasingExecutionAdapter(oms::ExecutionAdapter& inner, risk::RiskEngine& engine)
      : inner_(&inner), engine_(&engine) {}

  [[nodiscard]] bool submit(const events::exchange::NewOrderCommand& command) override {
    const bool accepted = inner_->submit(command);
    if (!accepted) {
      engine_->release_reservation(command.client_order_id);
    }
    return accepted;
  }
  [[nodiscard]] bool cancel(const events::exchange::CancelOrderCommand& command) override {
    return inner_->cancel(command);
  }
  [[nodiscard]] bool modify(const events::exchange::ModifyOrderCommand& command) override {
    return inner_->modify(command);
  }

 private:
  oms::ExecutionAdapter* inner_;
  risk::RiskEngine* engine_;
};

}  // namespace aegis::participant::app
