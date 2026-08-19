#pragma once

#include <cstdint>
#include <string>

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

/// For tests calling `RiskEngine::decide_order` directly with a hand-picked
/// `client_order_id` (no `OrderManager` involved): registers that id against
/// the armed leg `{strategy_id, proposal_id, leg_index}` names, then decides.
[[nodiscard]] inline aegis::participant::risk::OmsDecision decide_registered_order(
    aegis::participant::risk::RiskEngine& risk_engine, const std::string& strategy_id,
    const std::string& proposal_id, std::uint32_t leg_index, std::uint32_t instrument_id,
    aegis::events::exchange::Side side, std::int64_t quantity_units, std::uint64_t client_order_id,
    aegis::common::Nanos now_nanos) {
  risk_engine.register_pending_order_identity(client_order_id, strategy_id, proposal_id, leg_index);
  return risk_engine.decide_order(instrument_id, side, quantity_units, client_order_id, now_nanos);
}

/// For tests submitting through a real `OrderManager` (whose internal
/// `risk_gate_->decide()` call reaches `decide_order` synchronously, before
/// `submit_new_order` returns): peeks the id `OrderManager` is about to
/// assign, registers it against the named armed leg, then submits. Returns
/// the assigned `client_order_id`, exactly like `submit_new_order` itself.
[[nodiscard]] inline std::uint64_t submit_registered_new_order(
    aegis::participant::oms::OrderManager& manager,
    aegis::participant::risk::RiskEngine& risk_engine, const std::string& strategy_id,
    const std::string& proposal_id, std::uint32_t leg_index, std::uint32_t instrument_id,
    aegis::events::exchange::Side side, aegis::events::exchange::OrderType order_type,
    std::int64_t price_units, std::int64_t quantity_units,
    aegis::common::EventTime market_event_time = {}) {
  risk_engine.register_pending_order_identity(manager.next_client_order_id(), strategy_id,
                                              proposal_id, leg_index);
  return manager.submit_new_order(instrument_id, /*participant_id=*/1, side, order_type,
                                  price_units, quantity_units, market_event_time);
}

}  // namespace aegis::testing
