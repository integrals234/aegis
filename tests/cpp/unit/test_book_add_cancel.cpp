#include <gtest/gtest.h>

#include "cpp/exchange/order_book/book.hpp"

namespace {

using aegis::exchange::ClientOrderId;
using aegis::exchange::CommandSequence;
using aegis::exchange::InstrumentId;
using aegis::exchange::OrderBook;
using aegis::exchange::OrderId;
using aegis::exchange::OrderNode;
using aegis::exchange::OrderType;
using aegis::exchange::ParticipantId;
using aegis::exchange::PriceUnits;
using aegis::exchange::Priority;
using aegis::exchange::QuantityUnits;
using aegis::exchange::Side;

OrderNode make_order(std::uint64_t id, Side side, PriceUnits price, QuantityUnits quantity,
                     std::uint64_t command_sequence) {
  OrderNode node;
  node.order_id = OrderId{id};
  node.instrument_id = InstrumentId{1};
  node.participant_id = ParticipantId{1};
  node.client_order_id = ClientOrderId{id};
  node.side = side;
  node.order_type = OrderType::kLimit;
  node.price_units = price;
  node.original_quantity = quantity;
  node.remaining = quantity;
  node.priority = Priority::from(CommandSequence{command_sequence});
  return node;
}

TEST(OrderBookAddCancel, AddedOrderIsFindableAndRests) {
  OrderBook book{InstrumentId{1}};
  const auto order = make_order(1, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 1);
  book.add(order);

  const auto* found = book.find(OrderId{1});
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->remaining, QuantityUnits{10});
  EXPECT_EQ(book.best_bid(), PriceUnits{100});
  EXPECT_EQ(book.live_order_count(), 1U);
}

TEST(OrderBookAddCancel, LevelAggregateQuantityAccumulatesAcrossOrders) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 1));
  book.add(make_order(2, Side::kBuy, PriceUnits{100}, QuantityUnits{5}, 2));

  const auto* level = book.level_at(Side::kBuy, PriceUnits{100});
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->aggregate_quantity, QuantityUnits{15});
  EXPECT_EQ(level->order_count, 2U);
}

// AEGIS-027: within one price level, arrival order is preserved exactly —
// the golden sequence a later matching-consumption test also checks.
TEST(OrderBookAddCancel, GoldenSequencePreservesArrivalOrderWithinAPriceLevel) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 1));
  book.add(make_order(2, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 2));
  book.add(make_order(3, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 3));

  const auto ids = book.orders_at(Side::kBuy, PriceUnits{100});
  ASSERT_EQ(ids.size(), 3U);
  EXPECT_EQ(ids[0], OrderId{1});
  EXPECT_EQ(ids[1], OrderId{2});
  EXPECT_EQ(ids[2], OrderId{3});
}

TEST(OrderBookAddCancel, BestBidIsHighestBestAskIsLowest) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 1));
  book.add(make_order(2, Side::kBuy, PriceUnits{105}, QuantityUnits{10}, 2));
  book.add(make_order(3, Side::kSell, PriceUnits{110}, QuantityUnits{10}, 3));
  book.add(make_order(4, Side::kSell, PriceUnits{108}, QuantityUnits{10}, 4));

  EXPECT_EQ(book.best_bid(), PriceUnits{105});
  EXPECT_EQ(book.best_ask(), PriceUnits{108});
}

TEST(OrderBookAddCancel, CancelUnknownOrderReturnsNullopt) {
  OrderBook book{InstrumentId{1}};
  EXPECT_FALSE(book.cancel(OrderId{999}).has_value());
}

// AEGIS-030: cancellation verifies state, quantities and (via the returned
// snapshot) what an engine would need to emit a termination event.
TEST(OrderBookAddCancel, CancelRemovesOrderAndUpdatesAggregates) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 1));
  book.add(make_order(2, Side::kBuy, PriceUnits{100}, QuantityUnits{5}, 2));

  const auto removed = book.cancel(OrderId{1});
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed.value_or(OrderNode{}).remaining, QuantityUnits{10});

  EXPECT_EQ(book.find(OrderId{1}), nullptr);
  const auto* level = book.level_at(Side::kBuy, PriceUnits{100});
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->aggregate_quantity, QuantityUnits{5});
  EXPECT_EQ(level->order_count, 1U);
}

// AEGIS-034 (structural half): cancelling the last order at a price erases
// the level rather than leaving a zero-quantity level retained in the index.
TEST(OrderBookAddCancel, CancelingLastOrderAtALevelErasesTheLevel) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 1));

  book.cancel(OrderId{1});

  EXPECT_EQ(book.level_at(Side::kBuy, PriceUnits{100}), nullptr);
  EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBookAddCancel, CancelUnlinksFromMiddleOfQueueWithoutScanningTheQueue) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 1));
  book.add(make_order(2, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 2));
  book.add(make_order(3, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 3));

  book.cancel(OrderId{2});  // remove the middle order directly by id

  const auto ids = book.orders_at(Side::kBuy, PriceUnits{100});
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids[0], OrderId{1});
  EXPECT_EQ(ids[1], OrderId{3});
}

TEST(OrderBookAddCancel, SlabSlotIsReusedAfterCancel) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 1));
  book.cancel(OrderId{1});
  EXPECT_EQ(book.live_order_count(), 0U);

  book.add(make_order(2, Side::kBuy, PriceUnits{100}, QuantityUnits{5}, 2));
  EXPECT_EQ(book.live_order_count(), 1U);
  const auto* found = book.find(OrderId{2});
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->remaining, QuantityUnits{5});
}

TEST(OrderBookAddCancel, FindLiveByClientIdReflectsAddAndCancel) {
  OrderBook book{InstrumentId{1}};
  const auto order = make_order(1, Side::kBuy, PriceUnits{100}, QuantityUnits{10}, 1);
  book.add(order);

  auto found = book.find_live_by_client_id(order.participant_id, order.client_order_id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found, OrderId{1});

  book.cancel(OrderId{1});
  EXPECT_FALSE(
      book.find_live_by_client_id(order.participant_id, order.client_order_id).has_value());
}

}  // namespace
