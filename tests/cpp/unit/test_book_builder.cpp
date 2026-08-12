#include <gtest/gtest.h>

#include "cpp/participant/book_builder/book_builder.hpp"

namespace {

using aegis::events::exchange::Side;
using aegis::events::market_data::BookDeltaEvent;
using aegis::events::market_data::BookLevelEntry;
using aegis::events::market_data::BookSnapshotEvent;
using aegis::events::market_data::DeltaKind;
using aegis::participant::book::BookBuilder;
using aegis::participant::book::PriceLevelView;

BookSnapshotEvent make_snapshot() {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = 1;
  snapshot.md_sequence = 1;
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 100, .quantity_units = 10, .order_id = 1});
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 99, .quantity_units = 20, .order_id = 2});
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kSell, .price_units = 101, .quantity_units = 15, .order_id = 3});
  return snapshot;
}

// AEGIS-064: snapshot fixtures reconstruct expected levels and quantities.
TEST(BookBuilder, ApplySnapshotReconstructsLevelsAndQuantities) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());

  EXPECT_EQ(book.best(Side::kBuy), (PriceLevelView{100, 10}));
  EXPECT_EQ(book.best(Side::kSell), (PriceLevelView{101, 15}));
  EXPECT_EQ(book.quantity_at(Side::kBuy, 99), 20);
  EXPECT_EQ(book.last_md_sequence(), 1U);
}

TEST(BookBuilder, LevelsReturnsBestFirstUpToDepth) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());
  const auto bids = book.levels(Side::kBuy, 5);
  ASSERT_EQ(bids.size(), 2U);
  EXPECT_EQ(bids[0], (PriceLevelView{100, 10}));
  EXPECT_EQ(bids[1], (PriceLevelView{99, 20}));
}

// AEGIS-066: order-level reconstruction -- add, modify, remove.
TEST(BookBuilder, OrderLifecycleFixturesReconstructOrderLevelState) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());

  BookDeltaEvent added;
  added.instrument_id = 1;
  added.md_sequence = 2;
  added.kind = DeltaKind::kOrderAdded;
  added.order_id = 4;
  added.side = Side::kBuy;
  added.price_units = 98;
  added.quantity_units = 5;
  book.apply_delta(added);

  ASSERT_TRUE(book.order(4).has_value());
  EXPECT_EQ(*book.order(4), (aegis::participant::book::OrderView{Side::kBuy, 98, 5}));
  EXPECT_EQ(book.quantity_at(Side::kBuy, 98), 5);

  BookDeltaEvent modified;
  modified.instrument_id = 1;
  modified.md_sequence = 3;
  modified.kind = DeltaKind::kOrderModified;
  modified.order_id = 4;
  modified.side = Side::kBuy;
  modified.price_units = 98;
  modified.quantity_units = 2;
  book.apply_delta(modified);

  EXPECT_EQ(book.order(4)->quantity_units, 2);
  EXPECT_EQ(book.quantity_at(Side::kBuy, 98), 2);

  BookDeltaEvent removed;
  removed.instrument_id = 1;
  removed.md_sequence = 4;
  removed.kind = DeltaKind::kOrderRemoved;
  removed.order_id = 4;
  removed.side = Side::kBuy;
  book.apply_delta(removed);

  EXPECT_FALSE(book.order(4).has_value());
  EXPECT_FALSE(book.quantity_at(Side::kBuy, 98).has_value());
  EXPECT_EQ(book.last_md_sequence(), 4U);
}

// AEGIS-067: price-level (aggregated-only) reconstruction -- no order_id ever
// supplied, exercising kPriceLevelSet exclusively.
TEST(BookBuilder, AggregatedOnlyDeltasReconstructPriceLevels) {
  BookBuilder book(1);

  BookDeltaEvent set_bid;
  set_bid.instrument_id = 1;
  set_bid.md_sequence = 1;
  set_bid.kind = DeltaKind::kPriceLevelSet;
  set_bid.side = Side::kBuy;
  set_bid.price_units = 100;
  set_bid.quantity_units = 50;
  book.apply_delta(set_bid);

  EXPECT_EQ(book.quantity_at(Side::kBuy, 100), 50);
  EXPECT_FALSE(book.order(0).has_value());  // No order identity in this mode.

  BookDeltaEvent clear_bid;
  clear_bid.instrument_id = 1;
  clear_bid.md_sequence = 2;
  clear_bid.kind = DeltaKind::kPriceLevelSet;
  clear_bid.side = Side::kBuy;
  clear_bid.price_units = 100;
  clear_bid.quantity_units = 0;
  book.apply_delta(clear_bid);

  EXPECT_FALSE(book.quantity_at(Side::kBuy, 100).has_value());
  EXPECT_FALSE(book.best(Side::kBuy).has_value());
}

TEST(BookBuilder, ApplySnapshotDiscardsPriorState) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());
  ASSERT_TRUE(book.best(Side::kBuy).has_value());

  BookSnapshotEvent fresh;
  fresh.instrument_id = 1;
  fresh.md_sequence = 10;
  book.apply_snapshot(fresh);

  EXPECT_FALSE(book.best(Side::kBuy).has_value());
  EXPECT_FALSE(book.best(Side::kSell).has_value());
  EXPECT_EQ(book.last_md_sequence(), 10U);
}

TEST(BookBuilder, ModifyingAnUnknownOrderIsIgnored) {
  BookBuilder book(1);
  BookDeltaEvent modify_unknown;
  modify_unknown.instrument_id = 1;
  modify_unknown.md_sequence = 1;
  modify_unknown.kind = DeltaKind::kOrderModified;
  modify_unknown.order_id = 999;
  book.apply_delta(modify_unknown);
  EXPECT_FALSE(book.order(999).has_value());
  EXPECT_FALSE(book.best(Side::kBuy).has_value());
}

}  // namespace
