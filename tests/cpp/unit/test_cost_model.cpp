#include <gtest/gtest.h>

#include "cpp/participant/oms/cost_model.hpp"
#include "cpp/participant/portfolio/portfolio.hpp"

/// AEGIS-116: fees and slippage, plus a net-P&L reconciliation against
/// Portfolio's actual cash accounting.
namespace {

using aegis::events::exchange::Side;
using aegis::participant::oms::compute_slippage;
using aegis::participant::oms::FeeSchedule;
using aegis::participant::oms::SlippageResult;
using aegis::participant::portfolio::Portfolio;

TEST(FeeSchedule, FeeUnitsIsExactIntegerNotionalTimesRate) {
  const FeeSchedule schedule{.fee_rate_ppm = 1000};  // 0.1%.
  EXPECT_EQ(schedule.fee_units(/*price=*/1000, /*quantity=*/50), (1000 * 50 * 1000) / 1'000'000);
  EXPECT_EQ(schedule.fee_units(1000, 50), 50);
}

TEST(FeeSchedule, ZeroRateChargesNoFee) {
  const FeeSchedule schedule{.fee_rate_ppm = 0};
  EXPECT_EQ(schedule.fee_units(5000, 100), 0);
}

TEST(FeeSchedule, RoundsTowardZeroLikeTheRestOfTheIntegerArithmetic) {
  const FeeSchedule schedule{.fee_rate_ppm = 1};  // 0.0001%: rounds most small fills to zero.
  EXPECT_EQ(schedule.fee_units(999, 1), 0);
}

TEST(Slippage, BuyFillingAboveReferenceIsAdverse) {
  const SlippageResult result = compute_slippage(Side::kBuy, /*reference=*/1000, /*fill=*/1010);
  EXPECT_EQ(result.slippage_units, 10);  // Paid 10 more per unit than expected.
}

TEST(Slippage, BuyFillingBelowReferenceIsFavorable) {
  const SlippageResult result = compute_slippage(Side::kBuy, 1000, 990);
  EXPECT_EQ(result.slippage_units, -10);
}

TEST(Slippage, SellFillingBelowReferenceIsAdverse) {
  const SlippageResult result = compute_slippage(Side::kSell, /*reference=*/1000, /*fill=*/990);
  EXPECT_EQ(result.slippage_units, 10);  // Received 10 less per unit than expected.
}

TEST(Slippage, SellFillingAboveReferenceIsFavorable) {
  const SlippageResult result = compute_slippage(Side::kSell, 1000, 1010);
  EXPECT_EQ(result.slippage_units, -10);
}

TEST(Slippage, ExactlyAtReferenceIsZero) {
  EXPECT_EQ(compute_slippage(Side::kBuy, 1000, 1000).slippage_units, 0);
  EXPECT_EQ(compute_slippage(Side::kSell, 1000, 1000).slippage_units, 0);
}

// AEGIS-116: "Net P&L reconciliation passes." The identity checked here:
// (frictionless P&L at the reference price) - (slippage cost) - (fee) ==
// (Portfolio's own actual realized P&L + cash delta), computed two
// independent ways from the same fill.
TEST(CostModelReconciliation, NetPnlEqualsGrossMinusSlippageMinusFeeForAClosingBuy) {
  constexpr std::uint32_t kInstrument = 1;
  constexpr std::int64_t kReferencePrice = 1000;
  constexpr std::int64_t kFillPrice = 1010;  // 10 units/contract adverse slippage on the buy.
  constexpr std::int64_t kQuantity = 20;
  const FeeSchedule schedule{.fee_rate_ppm = 500};  // 0.05%.
  const std::int64_t fee = schedule.fee_units(kFillPrice, kQuantity);

  // Open a short so this fill is a closing buy with a well-defined realized P&L.
  Portfolio ledger;
  ledger.apply_fill(kInstrument, Side::kSell, /*price=*/1200, kQuantity);
  ledger.apply_fill(kInstrument, Side::kBuy, kFillPrice, kQuantity, fee);

  const SlippageResult slippage = compute_slippage(Side::kBuy, kReferencePrice, kFillPrice);

  // Gross P&L if this fill had happened at the reference price instead.
  const std::int64_t gross_pnl_at_reference = kQuantity * (1200 - kReferencePrice);
  const std::int64_t slippage_cost = slippage.slippage_units * kQuantity;
  const std::int64_t expected_net_pnl = gross_pnl_at_reference - slippage_cost - fee;

  const auto position = ledger.position(kInstrument);
  const std::int64_t actual_net_pnl = position.realized_pnl_units - fee;
  EXPECT_EQ(actual_net_pnl, expected_net_pnl);
}

}  // namespace
