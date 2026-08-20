#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/events/exchange_messages.hpp"
#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/risk/risk_engine.hpp"

/// Test-only convenience for the M5 closure repair's exact-identity seam
/// (`cpp/participant/risk/risk_engine.hpp`'s "Reservation timing and exact
/// identity" section): `RiskEngine::decide_order` no longer matches a
/// pending leg by economics -- a caller must first register the exact
/// `client_order_id` an order will carry against the armed leg it is for.
/// Production code (`cpp/participant/app/participant_run.cpp`'s
/// `execute_leg`) does this by peeking `OrderManager::next_client_order_id()`
/// immediately before `submit_new_order`. Tests that drive the seam
/// directly, or drive `OrderManager` without going through `execute_leg`,
/// need the identical two-step sequence; these helpers keep each call site
/// to one line instead of duplicating the peek-then-register pattern.
namespace aegis::testing {

/// Stages one constituent for `leg_index` with the economics the caller will
/// submit under, so a test can build a staging list without repeating the
/// designated-initializer boilerplate.
[[nodiscard]] inline aegis::participant::risk::StagedOrderIdentity staged_leg(
    std::uint64_t client_order_id, std::uint32_t leg_index, std::uint32_t instrument_id,
    aegis::events::exchange::Side side, std::int64_t quantity_units) {
  return aegis::participant::risk::StagedOrderIdentity{.client_order_id = client_order_id,
                                                       .leg_index = leg_index,
                                                       .instrument_id = instrument_id,
                                                       .side = side,
                                                       .quantity_units = quantity_units};
}

/// Stages every constituent of a committed proposal and takes its one
/// release authorization -- the composition root's own sequence
/// (`participant_run.cpp`), condensed for tests that drive `RiskEngine`
/// directly. Returns the release decision so a caller can assert on it.
inline aegis::participant::risk::ProposalReleaseDecision stage_and_authorize(
    aegis::participant::risk::RiskEngine& risk_engine, const std::string& strategy_id,
    const std::string& proposal_id,
    const std::vector<aegis::participant::risk::StagedOrderIdentity>& staged,
    aegis::common::Nanos now_nanos = 0) {
  risk_engine.stage_proposal_release(strategy_id, proposal_id, staged);
  return risk_engine.authorize_proposal_release(strategy_id, proposal_id, now_nanos);
}

/// For the many single-leg tests: stages that one leg, authorizes the
/// proposal, then consumes the authorization with `decide_order`. A
/// single-leg proposal's release epoch is degenerate (one constituent), so
/// this is exactly the multi-leg sequence with N = 1 -- not a shortcut
/// around it.
[[nodiscard]] inline aegis::participant::risk::OmsDecision decide_registered_order(
    aegis::participant::risk::RiskEngine& risk_engine, const std::string& strategy_id,
    const std::string& proposal_id, std::uint32_t leg_index, std::uint32_t instrument_id,
    aegis::events::exchange::Side side, std::int64_t quantity_units, std::uint64_t client_order_id,
    aegis::common::Nanos now_nanos) {
  stage_and_authorize(risk_engine, strategy_id, proposal_id,
                      {staged_leg(client_order_id, leg_index, instrument_id, side, quantity_units)},
                      now_nanos);
  return risk_engine.decide_order(instrument_id, side, quantity_units, client_order_id, now_nanos);
}

/// For tests submitting through a real `OrderManager` (whose internal
/// `risk_gate_->decide()` call reaches `decide_order` synchronously, before
/// `submit_new_order` returns). Assumes the proposal was ALREADY staged and
/// authorized -- for multi-leg proposals that must happen once, up front,
/// for every constituent together.
[[nodiscard]] inline std::uint64_t submit_staged_new_order(
    aegis::participant::oms::OrderManager& manager, std::uint32_t instrument_id,
    aegis::events::exchange::Side side, aegis::events::exchange::OrderType order_type,
    std::int64_t price_units, std::int64_t quantity_units,
    aegis::common::EventTime market_event_time = {}) {
  return manager.submit_new_order(instrument_id, /*participant_id=*/1, side, order_type,
                                  price_units, quantity_units, market_event_time);
}

/// Single-leg convenience for OMS-driven tests: stages the id the manager is
/// about to assign, authorizes, then submits.
[[nodiscard]] inline std::uint64_t submit_registered_new_order(
    aegis::participant::oms::OrderManager& manager,
    aegis::participant::risk::RiskEngine& risk_engine, const std::string& strategy_id,
    const std::string& proposal_id, std::uint32_t leg_index, std::uint32_t instrument_id,
    aegis::events::exchange::Side side, aegis::events::exchange::OrderType order_type,
    std::int64_t price_units, std::int64_t quantity_units,
    aegis::common::EventTime market_event_time = {}) {
  stage_and_authorize(
      risk_engine, strategy_id, proposal_id,
      {staged_leg(manager.next_client_order_id(), leg_index, instrument_id, side, quantity_units)});
  return manager.submit_new_order(instrument_id, /*participant_id=*/1, side, order_type,
                                  price_units, quantity_units, market_event_time);
}

}  // namespace aegis::testing
