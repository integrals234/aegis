#include <cstdint>
#include <tuple>

#include <gtest/gtest.h>

#include "cpp/exchange/matching/engine.hpp"
#include "cpp/exchange/matching/fifo_policy.hpp"

/// AEGIS-034: full fills remove the order and leave no dangling references
/// or incorrect aggregate quantities.
namespace {

using aegis::events::CommandSequence;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::exchange::FifoPolicy;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::MatchingEngine;
using aegis::exchange::OrderBook;
using aegis::exchange::OrderId;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;

InstrumentSpec make_spec() {
  return InstrumentSpec{
      .instrument_id = InstrumentId{1},
      .price_floor_units = PriceUnits{1000},
      .price_ceiling_units = PriceUnits{2000},
      .tick_size_units = 25,
      .min_quantity_units = QuantityUnits{50},
      .max_quantity_units = QuantityUnits{100000},
      .lot_size_units = 50,
  };
}

NewOrderCommand make_new_order(Side side, std::int64_t price, std::int64_t quantity,
                               std::uint64_t participant_id, std::uint64_t client_order_id) {
  return NewOrderCommand{
      .instrument_id = 1,
      .participant_id = participant_id,
      .client_order_id = client_order_id,
      .side = side,
      .order_type = OrderType::kLimit,
      .price_units = price,
      .quantity_units = quantity,
  };
}

/// Not a GTest fixture: see the identical note in test_limit_orders.cpp.
struct Harness {
  InstrumentSpec spec = make_spec();
  OrderBook book{InstrumentId{1}};
  FifoPolicy policy;
  MatchingEngine engine{policy};
  std::uint64_t next_sequence{1};

  void submit(Side side, std::int64_t price, std::int64_t quantity, std::uint64_t participant_id,
              std::uint64_t client_order_id) {
    std::ignore = engine.apply_new_order(
        book, spec, make_new_order(side, price, quantity, participant_id, client_order_id),
        CommandSequence{next_sequence++});
  }
};

TEST(FullFills, FullyFilledOrderIsRemovedFromTheIndex) {
  Harness h;
  h.submit(Side::kSell, 1000, 100, 10, 1);  // OrderId 1
  h.submit(Side::kBuy, 1000, 100, 20, 1);   // OrderId 2, fully fills OrderId 1

  EXPECT_EQ(h.book.find(OrderId{1}), nullptr) << "no dangling reference to the filled maker";
  EXPECT_EQ(h.book.find(OrderId{2}), nullptr) << "the fully filled taker must not rest either";
}

TEST(FullFills, EmptiedLevelIsErasedNotRetainedAtZero) {
  Harness h;
  h.submit(Side::kSell, 1000, 100, 10, 1);
  h.submit(Side::kBuy, 1000, 100, 20, 1);

  EXPECT_EQ(h.book.level_at(Side::kSell, PriceUnits{1000}), nullptr);
  EXPECT_FALSE(h.book.best_ask().has_value());
}

TEST(FullFills, SiblingOrdersAtTheSameLevelSurviveAFullFillOfTheHeadOrder) {
  Harness h;
  h.submit(Side::kSell, 1000, 50, 10, 1);  // OrderId 1: filled and removed
  h.submit(Side::kSell, 1000, 50, 11, 2);  // OrderId 2: stays resting
  h.submit(Side::kBuy, 1000, 50, 20, 1);   // fully consumes OrderId 1 only

  EXPECT_EQ(h.book.find(OrderId{1}), nullptr);
  const auto* survivor = h.book.find(OrderId{2});
  ASSERT_NE(survivor, nullptr) << "a full fill of the head order must not disturb its neighbor";
  EXPECT_EQ(survivor->remaining, QuantityUnits{50});

  const auto* level = h.book.level_at(Side::kSell, PriceUnits{1000});
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->aggregate_quantity, QuantityUnits{50})
      << "aggregate must reflect only the surviving order, not the filled one";
  EXPECT_EQ(level->order_count, 1U);

  const auto ids = h.book.orders_at(Side::kSell, PriceUnits{1000});
  ASSERT_EQ(ids.size(), 1U);
  EXPECT_EQ(ids[0], OrderId{2});
}

TEST(FullFills, MultipleFullFillsAcrossLevelsLeaveCorrectAggregates) {
  Harness h;
  h.submit(Side::kSell, 1000, 50, 10, 1);  // OrderId 1
  h.submit(Side::kSell, 1025, 50, 11, 2);  // OrderId 2
  h.submit(Side::kSell, 1050, 50, 12, 3);  // OrderId 3, survives
  h.submit(Side::kBuy, 1050, 100, 20, 1);  // sweeps OrderId 1 and 2 fully

  EXPECT_EQ(h.book.find(OrderId{1}), nullptr);
  EXPECT_EQ(h.book.find(OrderId{2}), nullptr);
  const auto* survivor = h.book.find(OrderId{3});
  ASSERT_NE(survivor, nullptr);
  EXPECT_EQ(survivor->remaining, QuantityUnits{50});

  EXPECT_EQ(h.book.level_at(Side::kSell, PriceUnits{1000}), nullptr);
  EXPECT_EQ(h.book.level_at(Side::kSell, PriceUnits{1025}), nullptr);
  EXPECT_EQ(h.book.best_ask(), PriceUnits{1050});
  EXPECT_EQ(h.book.live_order_count(), 1U);
}

TEST(FullFills, SlabSlotFromAFilledOrderIsReusedByALaterOrder) {
  Harness h;
  h.submit(Side::kSell, 1000, 100, 10, 1);  // OrderId 1: filled and released
  h.submit(Side::kBuy, 1000, 100, 20, 1);   // fills it

  const auto before = h.book.live_order_count();
  h.submit(Side::kSell, 1000, 100, 30, 1);  // OrderId 3: should reuse the freed slot cleanly
  EXPECT_EQ(h.book.live_order_count(), before + 1);
  const auto* fresh = h.book.find(OrderId{3});
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->remaining, QuantityUnits{100});
  EXPECT_EQ(fresh->participant_id.value(), 30U);
}

}  // namespace
