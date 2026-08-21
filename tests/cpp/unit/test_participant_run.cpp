#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "cpp/participant/app/participant_run.hpp"
#include "cpp/participant/oms/order_state.hpp"
#include "tests/cpp/optional_access.hpp"

/// The composition root's own proof, and the market-data event -> feed
/// handler -> reconstructed book -> microstructure feature -> generic
/// statistic -> OMS -> portfolio happy path Checkpoint 1 asks for
/// (ADR-0020).
namespace {

using aegis::participant::app::load_risk_limits_config;
using aegis::participant::app::run_builtin_scenario;
using aegis::participant::oms::OrderState;

std::filesystem::path write_temp_risk_config(const std::string& name, const std::string& content) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream file(path);
  file << content;
  file.close();
  return path;
}

TEST(ParticipantRun, BuiltinScenarioComposesEveryM3Layer) {
  const auto summary = run_builtin_scenario();

  // Feed handler -> book builder: the snapshot and both deltas landed.
  ASSERT_TRUE(summary.best_bid_price_units.has_value());
  EXPECT_EQ(aegis::test::checked(summary.best_bid_price_units),
            10'000);  // Unmoved: the delta touching it
  EXPECT_EQ(aegis::test::checked(summary.best_bid_quantity_units),
            30);  // was a modify to 30, not the new 9'990 order.
  ASSERT_TRUE(summary.best_ask_price_units.has_value());
  EXPECT_EQ(aegis::test::checked(summary.best_ask_price_units), 10'010);
  EXPECT_EQ(summary.last_md_sequence, 3U);

  // Reconstructed book -> microstructure feature (AEGIS-072): quantity-
  // weighted microprice over the same best bid/ask asserted above.
  ASSERT_TRUE(summary.microprice.has_value());
  EXPECT_NEAR(aegis::test::checked(summary.microprice),
              (((30.0 * 10'010.0) + (40.0 * 10'000.0)) / 70.0), 1e-9);

  // Feed handler -> generic statistic: three trades decoded and averaged.
  EXPECT_EQ(summary.trade_count, 3U);
  EXPECT_NEAR(summary.trade_price_rolling_mean, (10'005.0 + 10'007.0 + 10'004.0) / 3.0, 1e-9);

  // OMS: the built-in scenario runs its order to a terminal Filled state.
  EXPECT_EQ(summary.final_order_state, static_cast<std::uint8_t>(OrderState::kFilled));

  // Portfolio: the fill the OMS lifecycle represents is reflected.
  EXPECT_EQ(summary.position_quantity_units, 10);
  EXPECT_EQ(summary.position_average_price_units, 10'005);
  EXPECT_EQ(summary.cash_units, (-10'005 * 10) - 1);
}

TEST(ParticipantRun, BuiltinScenarioIsDeterministic) {
  const auto first = run_builtin_scenario();
  const auto second = run_builtin_scenario();
  EXPECT_EQ(first.best_bid_price_units, second.best_bid_price_units);
  EXPECT_EQ(first.trade_price_rolling_mean, second.trade_price_rolling_mean);
  EXPECT_EQ(first.final_order_state, second.final_order_state);
  EXPECT_EQ(first.realized_pnl_units, second.realized_pnl_units);
}

// ---------------------------------------------------------------------------
// M5 closure repair, N2: load_risk_limits_config rejects a non-positive
// configured order-quantity limit at the narrowest canonical boundary,
// rather than letting RiskEngine discover the hazard at decision time.
// ---------------------------------------------------------------------------

TEST(LoadRiskLimitsConfig, RejectsANegativeConfiguredOrderQuantityLimit) {
  const auto path =
      write_temp_risk_config("risk_limits_negative_cap.json",
                             R"({"order_quantity_limits":{"1":{"max_order_quantity_units":-100,)"
                             R"("resize_on_breach":true}}})");
  EXPECT_THROW({ static_cast<void>(load_risk_limits_config(path.string())); }, std::runtime_error);
}

TEST(LoadRiskLimitsConfig, RejectsAZeroConfiguredOrderQuantityLimit) {
  const auto path = write_temp_risk_config(
      "risk_limits_zero_cap.json", R"({"order_quantity_limits":{"1":{"max_order_quantity_units":0,)"
                                   R"("resize_on_breach":true}}})");
  EXPECT_THROW({ static_cast<void>(load_risk_limits_config(path.string())); }, std::runtime_error);
}

TEST(LoadRiskLimitsConfig, RejectsANegativeCapEvenWithResizeDisabled) {
  // The hazard is specific to resize_on_breach == true, but the config
  // itself is still nonsensical (a cap that can never be satisfied by any
  // positive quantity) with resize disabled, so it is rejected either way.
  const auto path =
      write_temp_risk_config("risk_limits_negative_cap_no_resize.json",
                             R"({"order_quantity_limits":{"1":{"max_order_quantity_units":-1,)"
                             R"("resize_on_breach":false}}})");
  EXPECT_THROW({ static_cast<void>(load_risk_limits_config(path.string())); }, std::runtime_error);
}

TEST(LoadRiskLimitsConfig, AcceptsAPositiveConfiguredOrderQuantityLimit) {
  const auto path =
      write_temp_risk_config("risk_limits_valid_cap.json",
                             R"({"order_quantity_limits":{"1":{"max_order_quantity_units":100,)"
                             R"("resize_on_breach":true}}})");
  const auto config = load_risk_limits_config(path.string());
  ASSERT_TRUE(config.order_quantity_limits.contains(1));
  EXPECT_EQ(config.order_quantity_limits.at(1).max_order_quantity_units, 100);
}

TEST(LoadRiskLimitsConfig, AcceptsAConfigWithNoOrderQuantityLimitsAtAll) {
  const auto path = write_temp_risk_config("risk_limits_empty.json", R"({})");
  EXPECT_NO_THROW({ static_cast<void>(load_risk_limits_config(path.string())); });
}

}  // namespace
