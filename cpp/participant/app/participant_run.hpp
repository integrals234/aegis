#pragma once

#include <cstdint>
#include <optional>

/// The participant composition root (ADR-0020).
///
/// `cpp-participant-app` is the only participant layer permitted to see feed
/// handler, book builder, statistics, OMS and portfolio at once, mirroring
/// `cpp-exchange-app`'s role on the exchange side (ADR-0012). It has no edge
/// to any `cpp-exchange-*` layer: everything it composes here is built from
/// participant-domain wire types directly (`cpp/events/market_data_messages.hpp`,
/// `cpp/events/exchange_messages.hpp`), never from a live `ExchangeNode`. Slice
/// 5 wires a real M1 exchange to this pipeline through a test-only
/// integration harness in `tests/` (outside `covered_roots`), not here.
namespace aegis::participant::app {

/// The outcome of `run_builtin_scenario()`: one number from each composed
/// layer, so a caller (the CLI, or a test) can prove the whole pipeline ran
/// -- feed decode, book reconstruction, a generic rolling statistic, an OMS
/// lifecycle transition, and a portfolio fill -- without depending on any of
/// those layers' internal types itself.
struct RunSummary {
  std::uint32_t instrument_id{0};
  std::optional<std::int64_t> best_bid_price_units;
  std::optional<std::int64_t> best_bid_quantity_units;
  std::optional<std::int64_t> best_ask_price_units;
  std::optional<std::int64_t> best_ask_quantity_units;
  std::uint64_t last_md_sequence{0};

  std::uint32_t trade_count{0};
  double trade_price_rolling_mean{0.0};

  std::uint8_t final_order_state{0};  ///< OrderState, as its underlying integer.

  std::int64_t position_quantity_units{0};
  std::int64_t position_average_price_units{0};
  std::int64_t realized_pnl_units{0};
  std::int64_t unrealized_pnl_units{0};
  std::int64_t cash_units{0};
};

/// Runs a small, fully deterministic, hand-built scenario through
/// feed handler -> book builder -> statistics -> OMS -> portfolio and
/// returns one summary value per layer. Deterministic because every input is
/// literal data constructed in this function, not read from a clock or the
/// environment -- repeated calls produce byte-identical results.
[[nodiscard]] RunSummary run_builtin_scenario();

}  // namespace aegis::participant::app
