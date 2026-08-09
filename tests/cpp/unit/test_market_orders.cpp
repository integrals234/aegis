#include <cstdint>

#include <gtest/gtest.h>

#include "cpp/exchange/matching/engine.hpp"
#include "cpp/exchange/matching/fifo_policy.hpp"

/// AEGIS-029: market orders execute against available liquidity with
/// explicit residual handling. A market order is accepted, never rejected
/// for lack of liquidity — an empty book is an execution outcome (whole
/// quantity terminated as residual), not a validation failure.
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

NewOrderCommand make_limit(Side side, std::int64_t price, std::int64_t quantity,
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

NewOrderCommand make_market(Side side, std::int64_t quantity, std::uint64_t participant_id = 1,
                            std::uint64_t client_order_id = 1) {
  return NewOrderCommand{
      .instrument_id = 1,
      .participant_id = participant_id,
      .client_order_id = client_order_id,
      .side = side,
      .order_type = OrderType::kMarket,
      .price_units = 0,
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

  std::vector<EmittedEvent> submit(const NewOrderCommand& command) {
    return engine.apply_new_order(book, spec, command, CommandSequence{next_sequence++});
  }
};

TEST(MarketOrders, EmptyBookIsAcceptedWithNoTradeAndTheWholeQuantityResidualCanceled) {
  Harness h;
  const auto events = h.submit(make_market(Side::kBuy, 100));

  // Accepted, then immediately Terminated(kResidualCanceled) — no Trade, and
  // critically no OrderRejected: an empty book is never a validation failure.
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderAccepted);
  EXPECT_EQ(events[1].message_type, MessageType::kOrderTerminated);
  EXPECT_NE(events[0].message_type, MessageType::kOrderRejected);
  EXPECT_NE(events[1].message_type, MessageType::kOrderRejected);

  EXPECT_EQ(decode_order_terminated(events[1].payload),
            (OrderTerminatedEvent{.causing_command_sequence = 1,
                                  .order_id = 1,
                                  .reason = TerminationReason::kResidualCanceled,
                                  .cancelled_quantity_delta_units = 100}));
  EXPECT_EQ(h.book.live_order_count(), 0U) << "a market order never rests, empty book or not";
}

TEST(MarketOrders, FullyFilledMarketOrderTerminatesAsFilledNotResidualCanceled) {
  Harness h;
  h.submit(make_limit(Side::kSell, 1000, 100, /*participant_id=*/10, /*client_order_id=*/1));
  const auto events = h.submit(make_market(Side::kBuy, 100, /*participant_id=*/20));

  // Accepted, Trade, maker Terminated(kFilled), taker Terminated(kFilled) —
  // exactly consumed, so the taker's reason is kFilled, not
  // kResidualCanceled: there is no residual to cancel.
  ASSERT_EQ(events.size(), 4U);
  EXPECT_EQ(
      decode_trade(events[1].payload).value_or(TradeEvent{.quantity_units = -1}).quantity_units,
      100);
  EXPECT_EQ(decode_order_terminated(events[3].payload),
            (OrderTerminatedEvent{.causing_command_sequence = 2,
                                  .order_id = 2,
                                  .reason = TerminationReason::kFilled,
                                  .cancelled_quantity_delta_units = 0}));
  EXPECT_EQ(h.book.live_order_count(), 0U);
}

TEST(MarketOrders, PartialFillResidualIsCanceledNotRested) {
  Harness h;
  h.submit(make_limit(Side::kSell, 1000, 50, /*participant_id=*/10, /*client_order_id=*/1));
  const auto events = h.submit(make_market(Side::kBuy, 100, /*participant_id=*/20));

  // Accepted, Trade(50), maker Terminated(kFilled), taker
  // Terminated(kResidualCanceled) for the unfilled 50 — the residual must
  // never rest (that would be the limit-order policy).
  ASSERT_EQ(events.size(), 4U);
  EXPECT_EQ(
      decode_trade(events[1].payload).value_or(TradeEvent{.quantity_units = -1}).quantity_units,
      50);
  EXPECT_EQ(decode_order_terminated(events[3].payload),
            (OrderTerminatedEvent{.causing_command_sequence = 2,
                                  .order_id = 2,
                                  .reason = TerminationReason::kResidualCanceled,
                                  .cancelled_quantity_delta_units = 50}));

  EXPECT_EQ(h.book.live_order_count(), 0U) << "the market order's residual must not rest";
  EXPECT_FALSE(h.book.best_bid().has_value());
}

TEST(MarketOrders, SweepsMultipleLevelsUntilQuantityIsExhausted) {
  Harness h;
  h.submit(make_limit(Side::kSell, 1000, 50, 10, 1));
  h.submit(make_limit(Side::kSell, 1025, 50, 11, 2));
  const auto events = h.submit(make_market(Side::kBuy, 100, 20));

  int trade_count = 0;
  for (const auto& event : events) {
    if (event.message_type == MessageType::kTrade) {
      ++trade_count;
    }
  }
  EXPECT_EQ(trade_count, 2);
  EXPECT_EQ(h.book.live_order_count(), 0U);
}

TEST(MarketOrders, NonzeroPriceIsRejected) {
  Harness h;
  // make_market always sends price 0; construct one with a price explicitly.
  NewOrderCommand with_price = make_market(Side::kBuy, 100);
  with_price.price_units = 1000;
  const auto events = h.submit(with_price);

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderRejected);
  EXPECT_EQ(decode_order_rejected(events[0].payload).value_or(OrderRejectedEvent{}).reason,
            RejectReason::kPriceOnMarketOrder);
}

TEST(MarketOrders, RejectedMarketOrderConsumesNoOrderIdAndDoesNotTouchTheBook) {
  Harness h;
  h.submit(make_limit(Side::kSell, 1000, 50, 10, 1));  // OrderId 1

  NewOrderCommand with_price = make_market(Side::kBuy, 50);
  with_price.price_units = 1000;
  const auto rejected_events = h.submit(with_price);
  ASSERT_EQ(rejected_events.size(), 1U);
  EXPECT_EQ(rejected_events[0].message_type, MessageType::kOrderRejected);

  // The resting sell must be untouched, and the next accepted order must
  // still get OrderId 2 — the rejected market order consumed nothing.
  const auto accepted_events = h.submit(make_market(Side::kBuy, 50, 30));
  EXPECT_EQ(decode_order_accepted(accepted_events[0].payload)
                .value_or(OrderAcceptedEvent{.order_id = 0})
                .order_id,
            2U);
}

}  // namespace
