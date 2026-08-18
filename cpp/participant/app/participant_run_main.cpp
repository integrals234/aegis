#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "cpp/participant/app/participant_run.hpp"

/// `aegis_participant_run`: the participant composition root's CLI entry
/// point (ADR-0020), mirroring `aegis_exchange_replay` (M1) and
/// `aegis_replay_run` (M2).
///
/// With no `--fixture`, runs the built-in deterministic scenario and prints
/// one JSON summary line -- unchanged since Checkpoint 1, the shape
/// evidence generators drive against as the M3 participant pipeline grows.
///
/// With `--fixture PATH`, runs the AEGIS-237/ADR-0024 fixture-driven
/// recovery path: `--skip`/`--limit` select a step range exactly like
/// `aegis_exchange_replay`, `--snapshot-out PATH` writes a
/// `ParticipantSnapshot` after that range, `--restore-from PATH` starts
/// from a previously written one instead of empty state. One canonical
/// JSON line per applied step is printed, so two split invocations'
/// concatenated stdout is comparable byte for byte against one
/// uninterrupted invocation covering the same range
/// (`tests/replay/test_participant_recovery.py`).
namespace {

using aegis::participant::app::CalendarSpreadRunConfig;
using aegis::participant::app::CalendarSpreadRunResult;
using aegis::participant::app::FixtureRunResult;
using aegis::participant::app::load_risk_limits_config;
using aegis::participant::app::run_builtin_scenario;
using aegis::participant::app::run_calendar_spread_scenario;
using aegis::participant::app::run_participant_fixture;
using aegis::participant::app::RunSummary;
using aegis::participant::risk::RiskLimitsConfig;

struct Options {
  std::optional<std::string> fixture_path;
  std::uint64_t skip{0};
  std::optional<std::uint64_t> limit;
  std::optional<std::string> snapshot_out;
  std::optional<std::string> restore_from;
  /// AEGIS-076..081, ADR-0025: `--calendar-spread --stream PATH` runs the
  /// first M4 strategy instead of the fixture/builtin paths above.
  bool calendar_spread{false};
  std::optional<std::string> stream_path;
  /// AEGIS-120..138, ADR-0027: the M5 risk config the real RiskEngine
  /// enforces the calendar-spread path with. Required together with
  /// --calendar-spread -- there is no implicit-approve fallback in M5.
  std::optional<std::string> risk_config_path;
};

[[nodiscard]] std::unordered_map<std::string, std::function<void(Options&, std::string)>>
make_flag_setters() {
  return {
      {"--fixture",
       [](Options& options, std::string value) { options.fixture_path = std::move(value); }},
      {"--skip",
       [](Options& options, const std::string& value) { options.skip = std::stoull(value); }},
      {"--limit",
       [](Options& options, const std::string& value) { options.limit = std::stoull(value); }},
      {"--snapshot-out",
       [](Options& options, std::string value) { options.snapshot_out = std::move(value); }},
      {"--restore-from",
       [](Options& options, std::string value) { options.restore_from = std::move(value); }},
      {"--stream",
       [](Options& options, std::string value) { options.stream_path = std::move(value); }},
      {"--risk-config",
       [](Options& options, std::string value) { options.risk_config_path = std::move(value); }},
  };
}

[[nodiscard]] std::optional<Options> parse_args(int argc, char** argv) {
  static const auto setters = make_flag_setters();

  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (flag == "--calendar-spread") {
      options.calendar_spread = true;
      continue;
    }
    const auto found = setters.find(flag);
    if (found == setters.end()) {
      std::cerr << "unknown argument: " << flag << "\n";
      return std::nullopt;
    }
    if (i + 1 >= argc) {
      std::cerr << "missing value for " << flag << "\n";
      return std::nullopt;
    }
    ++i;
    found->second(options, argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  return options;
}

void print_builtin_summary(const RunSummary& summary) {
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
  out["microprice"] = summary.microprice.has_value() ? nlohmann::json(*summary.microprice)
                                                     : nlohmann::json(nullptr);
  out["trade_count"] = summary.trade_count;
  out["trade_price_rolling_mean"] = summary.trade_price_rolling_mean;
  out["final_order_state"] = summary.final_order_state;
  out["position_quantity_units"] = summary.position_quantity_units;
  out["position_average_price_units"] = summary.position_average_price_units;
  out["realized_pnl_units"] = summary.realized_pnl_units;
  out["unrealized_pnl_units"] = summary.unrealized_pnl_units;
  out["cash_units"] = summary.cash_units;

  std::cout << out.dump() << "\n";
}

int run(const Options& options) {
  if (options.calendar_spread) {
    if (!options.stream_path.has_value()) {
      std::cerr << "--calendar-spread requires --stream PATH\n";
      return 2;
    }
    if (!options.risk_config_path.has_value()) {
      std::cerr << "--calendar-spread requires --risk-config PATH (AEGIS-120: no implicit-approve "
                   "fallback)\n";
      return 2;
    }
    const RiskLimitsConfig risk_config = load_risk_limits_config(*options.risk_config_path);
    const CalendarSpreadRunResult result =
        run_calendar_spread_scenario(*options.stream_path, CalendarSpreadRunConfig{}, risk_config);
    for (const auto& line : result.lines) {
      std::cout << line << "\n";
    }
    return 0;
  }

  if (!options.fixture_path.has_value()) {
    print_builtin_summary(run_builtin_scenario());
    return 0;
  }

  const FixtureRunResult result =
      run_participant_fixture(*options.fixture_path, options.skip, options.limit,
                              options.restore_from, options.snapshot_out);
  for (const auto& line : result.lines) {
    std::cout << line << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = parse_args(argc, argv);
  if (!options.has_value()) {
    std::cerr << "usage: aegis_participant_run "
                 "[--fixture PATH [--skip N] [--limit N] [--snapshot-out PATH] "
                 "[--restore-from PATH]] | [--calendar-spread --stream PATH --risk-config PATH]\n";
    return 2;
  }
  try {
    return run(*options);
  } catch (const std::exception& error) {
    std::cerr << "aegis_participant_run: " << error.what() << "\n";
    return 2;
  }
}
