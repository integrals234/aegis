#include <optional>

#include <gtest/gtest.h>

#include "cpp/exchange/order_book/level_index.hpp"

namespace {

using aegis::exchange::MapLevelIndex;
using aegis::exchange::PriceUnits;

TEST(MapLevelIndex, LevelAtCreatesAnEmptyLevelOnFirstAccess) {
  MapLevelIndex index{/*descending=*/false};
  auto& level = index.level_at(PriceUnits{100});
  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.price, PriceUnits{100});
  EXPECT_EQ(index.level_count(), 1U);
}

TEST(MapLevelIndex, LevelAtIsIdempotentForTheSamePrice) {
  MapLevelIndex index{/*descending=*/false};
  auto& first = index.level_at(PriceUnits{100});
  first.order_count = 3;
  auto& second = index.level_at(PriceUnits{100});
  EXPECT_EQ(second.order_count, 3U);
  EXPECT_EQ(index.level_count(), 1U);
}

TEST(MapLevelIndex, FindReturnsNullptrForAnAbsentPrice) {
  MapLevelIndex index{/*descending=*/false};
  EXPECT_EQ(index.find(PriceUnits{100}), nullptr);
}

TEST(MapLevelIndex, AscendingBestPriceIsLowest) {
  MapLevelIndex index{/*descending=*/false};
  index.level_at(PriceUnits{110});
  index.level_at(PriceUnits{100});
  index.level_at(PriceUnits{120});
  EXPECT_EQ(index.best_price(), PriceUnits{100});
}

TEST(MapLevelIndex, DescendingBestPriceIsHighest) {
  MapLevelIndex index{/*descending=*/true};
  index.level_at(PriceUnits{110});
  index.level_at(PriceUnits{100});
  index.level_at(PriceUnits{120});
  EXPECT_EQ(index.best_price(), PriceUnits{120});
}

TEST(MapLevelIndex, EraseRemovesTheLevel) {
  MapLevelIndex index{/*descending=*/false};
  index.level_at(PriceUnits{100});
  index.erase(PriceUnits{100});
  EXPECT_EQ(index.find(PriceUnits{100}), nullptr);
  EXPECT_TRUE(index.empty());
}

TEST(MapLevelIndex, NextPriceAfterWalksInBestToWorstOrder) {
  MapLevelIndex ascending{/*descending=*/false};
  ascending.level_at(PriceUnits{100});
  ascending.level_at(PriceUnits{110});
  ascending.level_at(PriceUnits{120});
  EXPECT_EQ(ascending.next_price_after(PriceUnits{100}), PriceUnits{110});
  EXPECT_EQ(ascending.next_price_after(PriceUnits{120}), std::nullopt);

  MapLevelIndex descending{/*descending=*/true};
  descending.level_at(PriceUnits{100});
  descending.level_at(PriceUnits{110});
  descending.level_at(PriceUnits{120});
  EXPECT_EQ(descending.next_price_after(PriceUnits{120}), PriceUnits{110});
  EXPECT_EQ(descending.next_price_after(PriceUnits{100}), std::nullopt);
}

TEST(MapLevelIndex, BestPriceIsNulloptWhenEmpty) {
  MapLevelIndex index{/*descending=*/false};
  EXPECT_FALSE(index.best_price().has_value());
}

}  // namespace
