#include <gtest/gtest.h>

#include "cpp/participant/portfolio/portfolio.hpp"

/// AEGIS-118: position and cash accounting. Each scenario below is checked
/// against a hand-computed expected value -- a double-entry-style
/// reconciliation, not a property-based sweep (that arrives with slice 6's
/// completion of this requirement).
namespace {

using aegis::events::exchange::Side;
using aegis::participant::portfolio::Portfolio;
using aegis::participant::portfolio::Position;

constexpr std::uint32_t kInstrument = 1;

TEST(Portfolio, OpeningALongPositionSetsAveragePriceAndDebitsCash) {
  Portfolio ledger;
  ledger.apply_fill(kInstrument, Side::kBuy, /*price=*/100, /*quantity=*/10, /*fee=*/1);

  const Position pos = ledger.position(kInstrument);
  EXPECT_EQ(pos.quantity_units, 10);
  EXPECT_EQ(pos.average_price_units, 100);
  EXPECT_EQ(pos.realized_pnl_units, 0);
  EXPECT_EQ(ledger.cash_units(), -100 * 10 - 1);
}

TEST(Portfolio, AddingToALongPositionUpdatesVolumeWeightedAveragePrice) {
  Portfolio ledger;
  ledger.apply_fill(kInstrument, Side::kBuy, 100, 10);
  ledger.apply_fill(kInstrument, Side::kBuy, 110, 10);

  const Position pos = ledger.position(kInstrument);
  EXPECT_EQ(pos.quantity_units, 20);
  // (10*100 + 10*110) / 20 = 105.
  EXPECT_EQ(pos.average_price_units, 105);
  EXPECT_EQ(pos.realized_pnl_units, 0);
}

TEST(Portfolio, PartiallyClosingALongPositionAtAProfitRealizesPnl) {
  Portfolio ledger;
  ledger.apply_fill(kInstrument, Side::kBuy, 100, 10);
  ledger.apply_fill(kInstrument, Side::kSell, 120, 4);

  const Position pos = ledger.position(kInstrument);
  EXPECT_EQ(pos.quantity_units, 6);
  EXPECT_EQ(pos.average_price_units, 100);  // Reducing never moves the basis.
  EXPECT_EQ(pos.realized_pnl_units, 4 * (120 - 100));
}

TEST(Portfolio, FullyClosingAPositionFlattensAndZeroesAveragePrice) {
  Portfolio ledger;
  ledger.apply_fill(kInstrument, Side::kBuy, 100, 10);
  ledger.apply_fill(kInstrument, Side::kSell, 130, 10);

  const Position pos = ledger.position(kInstrument);
  EXPECT_EQ(pos.quantity_units, 0);
  EXPECT_EQ(pos.average_price_units, 0);
  EXPECT_EQ(pos.realized_pnl_units, 10 * (130 - 100));
}

TEST(Portfolio, ClosingThroughZeroFlipsAndOpensAtTheFlipFillsPrice) {
  Portfolio ledger;
  ledger.apply_fill(kInstrument, Side::kBuy, 100, 10);
  ledger.apply_fill(kInstrument, Side::kSell, 120, 15);  // Closes 10 long, opens 5 short.

  const Position pos = ledger.position(kInstrument);
  EXPECT_EQ(pos.quantity_units, -5);
  EXPECT_EQ(pos.average_price_units, 120);  // The flip fill's own price, not the old basis.
  EXPECT_EQ(pos.realized_pnl_units, 10 * (120 - 100));
}

TEST(Portfolio, ShortPositionRealizesPnlOnAFavorablePriceDrop) {
  Portfolio ledger;
  ledger.apply_fill(kInstrument, Side::kSell, 100, 10);  // Opens short 10 @ 100.
  ledger.apply_fill(kInstrument, Side::kBuy, 90, 10);    // Covers at a lower price: profit.

  const Position pos = ledger.position(kInstrument);
  EXPECT_EQ(pos.quantity_units, 0);
  EXPECT_EQ(pos.realized_pnl_units, 10 * (100 - 90));
}

TEST(Portfolio, UnrealizedPnlUsesOneFormulaForBothLongAndShort) {
  Portfolio ledger;
  ledger.apply_fill(kInstrument, Side::kBuy, 100, 10);
  EXPECT_EQ(ledger.unrealized_pnl_units(kInstrument, /*mark=*/110), 10 * (110 - 100));
  EXPECT_EQ(ledger.unrealized_pnl_units(kInstrument, /*mark=*/90), 10 * (90 - 100));

  Portfolio short_ledger;
  short_ledger.apply_fill(kInstrument, Side::kSell, 100, 10);
  EXPECT_EQ(short_ledger.unrealized_pnl_units(kInstrument, /*mark=*/90), 10 * (90 - 100) * -1);
  EXPECT_EQ(short_ledger.unrealized_pnl_units(kInstrument, /*mark=*/110), 10 * (110 - 100) * -1);
}

TEST(Portfolio, UnknownInstrumentReportsAFlatPosition) {
  Portfolio ledger;
  const Position pos = ledger.position(kInstrument);
  EXPECT_EQ(pos, Position{});
}

}  // namespace
