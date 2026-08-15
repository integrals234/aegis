#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

  /// AEGIS-072, the microstructure feature this happy path exercises
  /// end-to-end: reconstructed book -> quantity-weighted microprice.
  std::optional<double> microprice;

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

/// AEGIS-237; ADR-0024: drives `OrderManager` + `Portfolio` through a
/// committed JSON-Lines fixture of OMS/execution/portfolio steps, over the
/// half-open step range `[skip, skip + limit)` (`limit` defaults to "every
/// remaining step" exactly like `aegis_exchange_replay --skip`/`--limit`).
/// If `restore_from_path` is set, state begins from a restored
/// `ParticipantSnapshot` rather than empty -- this is the process-boundary
/// recovery proof: two invocations split at a step boundary, the second
/// restoring what the first snapshotted, must produce output identical to
/// one uninterrupted invocation covering the same overall range. If
/// `snapshot_out_path` is set, a `ParticipantSnapshot` capturing the state
/// after the applied range is written there.
///
/// Every risk decision inside this run is a fixed always-approve verdict
/// (a test/fixture double, per ADR-0023: no production `RiskGate`
/// implementation ships before M5) and every outbound adapter call is
/// answered by nothing -- the fixture's own "order_accepted"/
/// "order_rejected"/"trade"/"order_terminated" steps supply every inbound
/// event directly, exactly as `tests/cpp/unit/test_participant_exchange_
/// integration.cpp`'s `deliver()` helper does, so this scenario has no
/// dependency on any exchange or transport (AEGIS-119's environment
/// independence, exercised here as the recovery proof's own execution
/// path rather than restated).
///
/// Throws `std::runtime_error` for a missing/unreadable fixture, an
/// unrecognized step `kind`, or a snapshot that fails to restore --
/// callers (the CLI) translate that into a nonzero exit rather than
/// silently continuing on state that was never actually reconstructed.
struct FixtureRunResult {
  /// One canonical JSON line per step applied in *this* invocation's own
  /// range (not the restored range, if any) -- the deterministic,
  /// concatenation-comparable output `tests/replay/test_participant_
  /// recovery.py` checks byte for byte across a process boundary.
  std::vector<std::string> lines;
};

[[nodiscard]] FixtureRunResult run_participant_fixture(
    const std::string& fixture_path, std::uint64_t skip, std::optional<std::uint64_t> limit,
    const std::optional<std::string>& restore_from_path,
    const std::optional<std::string>& snapshot_out_path);

}  // namespace aegis::participant::app
