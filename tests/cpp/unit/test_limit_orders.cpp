#include <cstdint>

#include <gtest/gtest.h>

#include "cpp/exchange/matching/engine.hpp"
#include "cpp/exchange/matching/fifo_policy.hpp"

/// AEGIS-028 (limit orders: bid, ask, crossing, noncrossing, invalid input)
/// and AEGIS-033 (partial fills conserve quantity, emit fill events).
///
/// Decoded events are compared as whole structs via `EXPECT_EQ(optional,
/// expected)` rather than by extracting individual fields with `->`/`*`/
/// `.value()`: `std::optional`'s heterogeneous `operator==` does the
/// comparison itself, so there is no unchecked access for a static analyzer
/// to flag, and a decode failure produces a clear "nullopt != expected"
/// diagnostic instead of a silently-defaulted field.
namespace {

using aegis::events::CommandSequence;
using aegis::events::MessageType;
using aegis::events::exchange::decode_order_accepted;
using aegis::events::exchange::decode_order_rejected;
using aegis::events::exchange::decode_order_terminated;
using aegis::events::exchange::decode_trade;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderAcceptedEvent;
using aegis::events::exchange::OrderRejectedEvent;
using aegis::events::exchange::OrderTerminatedEvent;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::RejectReason;
using aegis::events::exchange::Side;
using aegis::events::exchange::TerminationReason;
using aegis::events::exchange::TradeEvent;
using aegis::exchange::EmittedEvent;
using aegis::exchange::FifoPolicy;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::MatchingEngine;
using aegis::exchange::OrderBook;
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
                               std::uint64_t participant_id = 1,
                               std::uint64_t client_order_id = 1) {
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

/// Not a GTest fixture (no `TEST_F`, no inheritance): the codebase's existing
/// convention is a plain local helper per test, and a class deriving from
/// `::testing::Test` with a `protected:` section trips
/// cppcoreguidelines-non-private-member-variables-in-classes.
struct Harness {
  InstrumentSpec spec = make_spec();
  OrderBook book{InstrumentId{1}};
  FifoPolicy policy;
  MatchingEngine engine{policy};
  std::uint64_t next_sequence{1};

  std::vector<EmittedEvent> submit(const NewOrderCommand& command) {
    return engine.apply_new_order(book, spec, command, CommandSequence{next_sequence++});
  }
};

TEST(LimitOrders, NoncrossingBidRestsAndEmitsOnlyAccepted) {
  Harness h;
  const auto events = h.submit(make_new_order(Side::kBuy, 1000, 100));
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderAccepted);
  EXPECT_EQ(decode_order_accepted(events[0].payload),
            (OrderAcceptedEvent{.causing_command_sequence = 1,
                                .order_id = 1,
                                .instrument_id = 1,
                                .participant_id = 1,
                                .client_order_id = 1,
                                .side = Side::kBuy,
                                .order_type = OrderType::kLimit,
                                .price_units = 1000,
                                .quantity_units = 100}));

  EXPECT_EQ(h.book.best_bid(), PriceUnits{1000});
  EXPECT_EQ(h.book.live_order_count(), 1U);
}

TEST(LimitOrders, NoncrossingAskRests) {
  Harness h;
  const auto events = h.submit(make_new_order(Side::kSell, 1500, 100));
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderAccepted);
  EXPECT_EQ(h.book.best_ask(), PriceUnits{1500});
}

TEST(LimitOrders, TwoOrdersAtDifferentPricesBothRestWithoutCrossing) {
  Harness h;
  h.submit(make_new_order(Side::kBuy, 1000, 100, /*participant_id=*/1, /*client_order_id=*/1));
  h.submit(make_new_order(Side::kSell, 1100, 100, /*participant_id=*/1, /*client_order_id=*/2));

  EXPECT_EQ(h.book.best_bid(), PriceUnits{1000});
  EXPECT_EQ(h.book.best_ask(), PriceUnits{1100});
  EXPECT_EQ(h.book.live_order_count(), 2U);
}

TEST(LimitOrders, CrossingOrderFullyFillsAgainstASingleMaker) {
  Harness h;
  h.submit(make_new_order(Side::kSell, 1000, 100, /*participant_id=*/10, /*client_order_id=*/1));
  const auto events =
      h.submit(make_new_order(Side::kBuy, 1000, 100, /*participant_id=*/20, /*client_order_id=*/1));

  // Accepted, Trade, maker Terminated(kFilled), taker Terminated(kFilled).
  ASSERT_EQ(events.size(), 4U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderAccepted);
  EXPECT_EQ(events[1].message_type, MessageType::kTrade);
  EXPECT_EQ(events[2].message_type, MessageType::kOrderTerminated);
  EXPECT_EQ(events[3].message_type, MessageType::kOrderTerminated);

  EXPECT_EQ(decode_trade(events[1].payload), (TradeEvent{.causing_command_sequence = 2,
                                                         .instrument_id = 1,
                                                         .price_units = 1000,
                                                         .quantity_units = 100,
                                                         .maker_order_id = 1,
                                                         .taker_order_id = 2,
                                                         .maker_participant_id = 10,
                                                         .taker_participant_id = 20,
                                                         .taker_side = Side::kBuy}));
  EXPECT_EQ(decode_order_terminated(events[2].payload),
            (OrderTerminatedEvent{.causing_command_sequence = 2,
                                  .order_id = 1,
                                  .reason = TerminationReason::kFilled,
                                  .cancelled_quantity_delta_units = 0}));
  EXPECT_EQ(decode_order_terminated(events[3].payload),
            (OrderTerminatedEvent{.causing_command_sequence = 2,
                                  .order_id = 2,
                                  .reason = TerminationReason::kFilled,
                                  .cancelled_quantity_delta_units = 0}));

  EXPECT_EQ(h.book.live_order_count(), 0U);
  EXPECT_FALSE(h.book.best_bid().has_value());
  EXPECT_FALSE(h.book.best_ask().has_value());
}

TEST(LimitOrders, CrossingOrderTakesThePriceImprovement) {
  // A resting sell at 1000 is better than the buyer's limit of 1025; the
  // trade must print at the maker's (better) price, not the taker's limit.
  Harness h;
  h.submit(make_new_order(Side::kSell, 1000, 100, /*participant_id=*/1, /*client_order_id=*/1));
  const auto events =
      h.submit(make_new_order(Side::kBuy, 1025, 100, /*participant_id=*/1, /*client_order_id=*/2));

  const auto trade = decode_trade(events[1].payload);
  EXPECT_EQ(trade.value_or(TradeEvent{.price_units = -1}).price_units, 1000);
}

// AEGIS-033: the aggressor's quantity is only partially satisfied by the
// resting maker; the maker fills fully and terminates, while the aggressor's
// unfilled residual rests as a fresh resting order at its own limit.
TEST(LimitOrders, PartialFillRestsTheAggressorsResidual) {
  Harness h;
  h.submit(make_new_order(Side::kSell, 1000, 50, /*participant_id=*/10));
  const auto events = h.submit(make_new_order(Side::kBuy, 1000, 100, /*participant_id=*/20));

  // Accepted, Trade(50), maker Terminated(kFilled) — taker still has 50
  // remaining and rests, so there is no taker-Terminated event.
  ASSERT_EQ(events.size(), 3U);
  const auto trade = decode_trade(events[1].payload);
  EXPECT_EQ(trade.value_or(TradeEvent{.quantity_units = -1}).quantity_units, 50);

  EXPECT_EQ(h.book.live_order_count(), 1U);
  EXPECT_EQ(h.book.best_bid(), PriceUnits{1000});
  const auto ids = h.book.orders_at(Side::kBuy, PriceUnits{1000});
  ASSERT_EQ(ids.size(), 1U);
  const auto* resting = h.book.find(ids[0]);
  ASSERT_NE(resting, nullptr);
  EXPECT_EQ(resting->original_quantity, QuantityUnits{100});
  EXPECT_EQ(resting->cumulative_filled, QuantityUnits{50});
  EXPECT_EQ(resting->remaining, QuantityUnits{50});
}

// AEGIS-033: the aggressor sweeps a maker only partially, so the maker stays
// resting with reduced quantity and unchanged priority, while the aggressor
// fully fills and terminates.
TEST(LimitOrders, PartialFillLeavesTheMakerRestingWithReducedQuantity) {
  Harness h;
  h.submit(make_new_order(Side::kSell, 1000, 100, /*participant_id=*/10));
  const auto events = h.submit(make_new_order(Side::kBuy, 1000, 50, /*participant_id=*/20));

  // Accepted, Trade(50), taker Terminated(kFilled) — maker still has 50
  // remaining and stays resting, so there is no maker-Terminated event.
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[2].message_type, MessageType::kOrderTerminated);

  EXPECT_EQ(h.book.live_order_count(), 1U);
  const auto ids = h.book.orders_at(Side::kSell, PriceUnits{1000});
  ASSERT_EQ(ids.size(), 1U);
  const auto* maker = h.book.find(ids[0]);
  ASSERT_NE(maker, nullptr);
  EXPECT_EQ(maker->cumulative_filled, QuantityUnits{50});
  EXPECT_EQ(maker->remaining, QuantityUnits{50});

  const auto* level = h.book.level_at(Side::kSell, PriceUnits{1000});
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->aggregate_quantity, QuantityUnits{50});
}

TEST(LimitOrders, NonPositiveQuantityIsRejectedAndTouchesNothing) {
  Harness h;
  const auto events = h.submit(make_new_order(Side::kBuy, 1000, 0));
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderRejected);
  EXPECT_EQ(decode_order_rejected(events[0].payload),
            (OrderRejectedEvent{.causing_command_sequence = 1,
                                .instrument_id = 1,
                                .participant_id = 1,
                                .client_order_id = 1,
                                .reason = RejectReason::kNonPositiveQuantity}));
  EXPECT_EQ(h.book.live_order_count(), 0U);
}

TEST(LimitOrders, OffTickPriceIsRejected) {
  Harness h;
  const auto events = h.submit(make_new_order(Side::kBuy, 1010, 100));
  ASSERT_EQ(events.size(), 1U);
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kPriceNotOnTick);
  EXPECT_EQ(h.book.live_order_count(), 0U);
}

TEST(LimitOrders, OffLotQuantityIsRejected) {
  Harness h;
  const auto events = h.submit(make_new_order(Side::kBuy, 1000, 130));
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kQuantityNotOnLot);
}

TEST(LimitOrders, RejectedOrderDoesNotConsumeAnOrderId) {
  Harness h;
  h.submit(make_new_order(Side::kBuy, 1000, 0));  // rejected: nonpositive quantity
  const auto events = h.submit(make_new_order(Side::kBuy, 1000, 50));
  const auto accepted = decode_order_accepted(events[0].payload);
  EXPECT_EQ(accepted.value_or(OrderAcceptedEvent{.order_id = 0}).order_id, 1U)
      << "a rejected NewOrder must not consume an OrderId";
}

}  // namespace
