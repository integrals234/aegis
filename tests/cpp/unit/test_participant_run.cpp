#include <gtest/gtest.h>

#include "cpp/participant/app/participant_run.hpp"
#include "cpp/participant/oms/order_state.hpp"

/// The composition root's own proof, and the market-data event -> feed
/// handler -> reconstructed book -> microstructure feature -> generic
/// statistic -> OMS -> portfolio happy path Checkpoint 1 asks for
/// (ADR-0020).
namespace {

using aegis::participant::app::run_builtin_scenario;
using aegis::participant::oms::OrderState;

TEST(ParticipantRun, BuiltinScenarioComposesEveryM3Layer) {
  const auto summary = run_builtin_scenario();

  // Feed handler -> book builder: the snapshot and both deltas landed.
  ASSERT_TRUE(summary.best_bid_price_units.has_value());
  EXPECT_EQ(summary.best_bid_price_units.value(), 10'000);  // Unmoved: the delta touching it
  EXPECT_EQ(summary.best_bid_quantity_units.value(),
            30);  // was a modify to 30, not the new 9'990 order.
  ASSERT_TRUE(summary.best_ask_price_units.has_value());
  EXPECT_EQ(summary.best_ask_price_units.value(), 10'010);
  EXPECT_EQ(summary.last_md_sequence, 3U);

  // Reconstructed book -> microstructure feature (AEGIS-072): quantity-
  // weighted microprice over the same best bid/ask asserted above.
  ASSERT_TRUE(summary.microprice.has_value());
  EXPECT_NEAR(summary.microprice.value(), (((30.0 * 10'010.0) + (40.0 * 10'000.0)) / 70.0), 1e-9);

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

}  // namespace
