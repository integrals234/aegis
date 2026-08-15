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
  EXPECT_EQ(schedule.fee_units(/*price_units=*/1000, /*quantity_units=*/50),
            (1000 * 50 * 1000) / 1'000'000);
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
  const SlippageResult result =
      compute_slippage(Side::kBuy, /*reference_price_units=*/1000, /*fill_price_units=*/1010);
  EXPECT_EQ(result.slippage_units, 10);  // Paid 10 more per unit than expected.
}

TEST(Slippage, BuyFillingBelowReferenceIsFavorable) {
  const SlippageResult result = compute_slippage(Side::kBuy, 1000, 990);
  EXPECT_EQ(result.slippage_units, -10);
}

TEST(Slippage, SellFillingBelowReferenceIsAdverse) {
  const SlippageResult result =
      compute_slippage(Side::kSell, /*reference_price_units=*/1000, /*fill_price_units=*/990);
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

// AEGIS-116: "Net P&L reconciliation passes."
//
// The M3 closure audit found an earlier version of this test vacuous on the
// fee leg: it subtracted `fee` from BOTH the expected and the actual side, so
// the term cancelled and no fee was ever reconciled. It also never asserted
// cash, which is the only place a fee actually lands
// (`Portfolio::apply_fill`). Both are fixed here: cash is asserted directly,
// and the fee is reconciled by DIFFERENCE against a fee-free run of the same
// fills rather than by an identity that cancels.
TEST(CostModelReconciliation, FeeLandsInCashAndIsReconciledAgainstAFeeFreeRun) {
  constexpr std::uint32_t kInstrument = 1;
  constexpr std::int64_t kFillPrice = 1010;
  constexpr std::int64_t kQuantity = 20;
  const FeeSchedule schedule{.fee_rate_ppm = 500};  // 0.05%.
  const std::int64_t fee = schedule.fee_units(kFillPrice, kQuantity);
  ASSERT_GT(fee, 0) << "a zero fee would make this reconciliation vacuous";

  Portfolio with_fee;
  with_fee.apply_fill(kInstrument, Side::kSell, /*price_units=*/1200, kQuantity);
  with_fee.apply_fill(kInstrument, Side::kBuy, kFillPrice, kQuantity, fee);

  Portfolio without_fee;
  without_fee.apply_fill(kInstrument, Side::kSell, /*price_units=*/1200, kQuantity);
  without_fee.apply_fill(kInstrument, Side::kBuy, kFillPrice, kQuantity, /*fee_units=*/0);

  // The fee shows up in cash and nowhere else: realized P&L is gross of fees
  // by construction, so the two runs must agree on it and differ on cash by
  // exactly the fee.
  EXPECT_EQ(without_fee.cash_units() - with_fee.cash_units(), fee);
  EXPECT_EQ(with_fee.position(kInstrument).realized_pnl_units,
            without_fee.position(kInstrument).realized_pnl_units);

  // And cash itself is asserted outright, not merely by difference:
  // +1200*20 received opening the short, -1010*20 paid closing it, -fee.
  EXPECT_EQ(with_fee.cash_units(), (1200 * kQuantity) - (kFillPrice * kQuantity) - fee);
}

TEST(CostModelReconciliation, NetPnlEqualsGrossAtReferenceMinusSlippageMinusFee) {
  constexpr std::uint32_t kInstrument = 1;
  constexpr std::int64_t kReferencePrice = 1000;
  constexpr std::int64_t kFillPrice = 1010;  // 10/unit adverse slippage on the buy.
  constexpr std::int64_t kQuantity = 20;
  const FeeSchedule schedule{.fee_rate_ppm = 500};
  const std::int64_t fee = schedule.fee_units(kFillPrice, kQuantity);

  Portfolio ledger;
  ledger.apply_fill(kInstrument, Side::kSell, /*price_units=*/1200, kQuantity);
  ledger.apply_fill(kInstrument, Side::kBuy, kFillPrice, kQuantity, fee);

  const SlippageResult slippage = compute_slippage(Side::kBuy, kReferencePrice, kFillPrice);
  const std::int64_t slippage_cost = slippage.slippage_units * kQuantity;

  // Expected side: what the round trip would have earned at the reference
  // price, less what slippage cost, less the fee. Every term is computed
  // from the cost model and the fixture -- none from the ledger.
  const std::int64_t expected_net = (kQuantity * (1200 - kReferencePrice)) - slippage_cost - fee;

  // Actual side: the ledger's OWN realized P&L, which is gross of fees, with
  // the fee taken from CASH rather than re-subtracted from the same figure --
  // so the fee term cannot cancel between the two sides.
  const std::int64_t fee_paid = (1200 * kQuantity) - (kFillPrice * kQuantity) - ledger.cash_units();
  const std::int64_t actual_net = ledger.position(kInstrument).realized_pnl_units - fee_paid;

  EXPECT_EQ(fee_paid, fee);  // The ledger really charged the modelled fee.
  EXPECT_EQ(actual_net, expected_net);
}

}  // namespace
