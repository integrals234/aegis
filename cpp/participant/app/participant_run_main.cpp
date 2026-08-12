#include <iostream>

#include <nlohmann/json.hpp>

#include "cpp/participant/app/participant_run.hpp"

/// `aegis_participant_run`: the participant composition root's CLI entry
/// point (ADR-0020), mirroring `aegis_exchange_replay` (M1) and
/// `aegis_replay_run` (M2). Runs the built-in deterministic scenario and
/// prints one JSON summary line to stdout — the shape evidence generators
/// drive against as the M3 participant pipeline grows.
int main() {
  const aegis::participant::app::RunSummary summary =
      aegis::participant::app::run_builtin_scenario();

  nlohmann::json out;
  out["instrument_id"] = summary.instrument_id;
  out["best_bid_price_units"] = summary.best_bid_price_units.has_value()
                                    ? nlohmann::json(*summary.best_bid_price_units)
                                    : nlohmann::json(nullptr);
  out["best_bid_quantity_units"] = summary.best_bid_quantity_units.has_value()
                                       ? nlohmann::json(*summary.best_bid_quantity_units)
                                       : nlohmann::json(nullptr);
  out["best_ask_price_units"] = summary.best_ask_price_units.has_value()
                                    ? nlohmann::json(*summary.best_ask_price_units)
                                    : nlohmann::json(nullptr);
  out["best_ask_quantity_units"] = summary.best_ask_quantity_units.has_value()
                                       ? nlohmann::json(*summary.best_ask_quantity_units)
                                       : nlohmann::json(nullptr);
  out["last_md_sequence"] = summary.last_md_sequence;
  out["trade_count"] = summary.trade_count;
  out["trade_price_rolling_mean"] = summary.trade_price_rolling_mean;
  out["final_order_state"] = summary.final_order_state;
  out["position_quantity_units"] = summary.position_quantity_units;
  out["position_average_price_units"] = summary.position_average_price_units;
  out["realized_pnl_units"] = summary.realized_pnl_units;
  out["unrealized_pnl_units"] = summary.unrealized_pnl_units;
  out["cash_units"] = summary.cash_units;

  std::cout << out.dump() << "\n";
  return 0;
}
