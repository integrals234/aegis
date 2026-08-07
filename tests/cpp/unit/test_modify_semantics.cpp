#include <cstdint>

#include <gtest/gtest.h>

#include "cpp/exchange/matching/engine.hpp"
#include "cpp/exchange/matching/fifo_policy.hpp"

/// AEGIS-030 (cancel: state, quantities, emitted events), AEGIS-031
/// (quantity modifications: priority retained or lost exactly as
/// documented), AEGIS-032 (price modifications: old-priority removal,
/// new-level placement). ADR-0011 owns the semantics these tests pin.
namespace {

using aegis::events::CommandSequence;
using aegis::events::MessageType;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::decode_order_modified;
using aegis::events::exchange::decode_order_rejected;
using aegis::events::exchange::decode_order_replaced;
using aegis::events::exchange::decode_order_terminated;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderModifiedEvent;
using aegis::events::exchange::OrderRejectedEvent;
using aegis::events::exchange::OrderReplacedEvent;
using aegis::events::exchange::OrderTerminatedEvent;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::RejectReason;
using aegis::events::exchange::Side;
using aegis::events::exchange::TerminationReason;
using aegis::exchange::EmittedEvent;
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

/// Not a GTest fixture: see the identical note in test_limit_orders.cpp.
struct Harness {
  InstrumentSpec spec = make_spec();
  OrderBook book{InstrumentId{1}};
  FifoPolicy policy;
  MatchingEngine engine{policy};
  std::uint64_t next_sequence{1};

  std::vector<EmittedEvent> new_order(const NewOrderCommand& command) {
    return engine.apply_new_order(book, spec, command, CommandSequence{next_sequence++});
  }
  std::vector<EmittedEvent> cancel(const CancelOrderCommand& command) {
    return MatchingEngine::apply_cancel_order(book, command, CommandSequence{next_sequence++});
  }
  std::vector<EmittedEvent> modify(const ModifyOrderCommand& command) {
    return engine.apply_modify_order(book, spec, command, CommandSequence{next_sequence++});
  }
};

// -------------------------------------------------------------------- cancel

TEST(ModifySemantics, CancelEmitsTerminatedAndRemovesTheOrder) {
  Harness h;
  h.new_order(make_limit(Side::kBuy, 1000, 100, 10, 1));  // OrderId 1

  const auto events =
      h.cancel(CancelOrderCommand{.instrument_id = 1, .participant_id = 10, .order_id = 1});
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderTerminated);
  EXPECT_EQ(decode_order_terminated(events[0].payload),
            (OrderTerminatedEvent{.causing_command_sequence = 2,
                                  .order_id = 1,
                                  .reason = TerminationReason::kCanceled,
                                  .cancelled_quantity_delta_units = 100}));
  EXPECT_EQ(h.book.find(OrderId{1}), nullptr);
  EXPECT_EQ(h.book.live_order_count(), 0U);
}

TEST(ModifySemantics, CancelOfPartiallyFilledOrderReportsOnlyTheRemainder) {
  Harness h;
  h.new_order(make_limit(Side::kSell, 1000, 100, 10, 1));  // OrderId 1
  h.new_order(make_limit(Side::kBuy, 1000, 50, 20, 1));    // fills 50 of OrderId 1

  const auto events =
      h.cancel(CancelOrderCommand{.instrument_id = 1, .participant_id = 10, .order_id = 1});
  const auto terminated = decode_order_terminated(events[0].payload);
  EXPECT_EQ(terminated.value_or(OrderTerminatedEvent{.cancelled_quantity_delta_units = -1})
                .cancelled_quantity_delta_units,
            50)
      << "only the unfilled remainder is cancelled, not the original quantity";
}

TEST(ModifySemantics, CancelOfUnknownOrderIsRejected) {
  Harness h;
  const auto events =
      h.cancel(CancelOrderCommand{.instrument_id = 1, .participant_id = 10, .order_id = 999});
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(decode_order_rejected(events[0].payload),
            (OrderRejectedEvent{.causing_command_sequence = 1,
                                .instrument_id = 1,
                                .participant_id = 10,
                                .client_order_id = 0,
                                .order_id = 999,
                                .reason = RejectReason::kUnknownOrderId}));
}

TEST(ModifySemantics, CancelByAnyoneOtherThanTheOwnerIsRejected) {
  Harness h;
  h.new_order(make_limit(Side::kBuy, 1000, 100, 10, 1));  // OrderId 1, owned by participant 10

  const auto events =
      h.cancel(CancelOrderCommand{.instrument_id = 1, .participant_id = 999, .order_id = 1});
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kNotOrderOwner);
  EXPECT_NE(h.book.find(OrderId{1}), nullptr) << "a rejected cancel must not touch the order";
}

TEST(ModifySemantics, CancelOfAnAlreadyTerminatedOrderIsUnknownNotAlreadyTerminal) {
  Harness h;
  h.new_order(make_limit(Side::kBuy, 1000, 100, 10, 1));  // OrderId 1
  h.cancel(CancelOrderCommand{.instrument_id = 1, .participant_id = 10, .order_id = 1});

  // ADR-0011: no unbounded tombstone set, so a second cancel of the same id
  // is indistinguishable from one that never existed.
  const auto events =
      h.cancel(CancelOrderCommand{.instrument_id = 1, .participant_id = 10, .order_id = 1});
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kUnknownOrderId);
}

// ------------------------------------------------------------ quantity decrease

// AEGIS-031: a quantity decrease is applied in place and retains priority —
// proved by a following aggressor that must still consume the decreased
// order before a later-arriving order at the same price.
TEST(ModifySemantics, QuantityDecreaseRetainsPriority) {
  Harness h;
  h.new_order(make_limit(Side::kSell, 1000, 100, 10, 1));  // OrderId 1: decreased below
  h.new_order(make_limit(Side::kSell, 1000, 50, 11, 2));   // OrderId 2: arrived after

  const auto modify_events = h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                         .participant_id = 10,
                                                         .order_id = 1,
                                                         .new_price_units = 1000,
                                                         .new_quantity_units = 50});
  ASSERT_EQ(modify_events.size(), 1U);
  EXPECT_EQ(modify_events[0].message_type, MessageType::kOrderModified);
  EXPECT_EQ(decode_order_modified(modify_events[0].payload),
            (OrderModifiedEvent{.causing_command_sequence = 3,
                                .order_id = 1,
                                .new_remaining_units = 50,
                                .cancelled_quantity_delta_units = 50}));

  // An aggressor for 50 must trade against OrderId 1 (still at the head of
  // the queue), not OrderId 2, which would happen if the decrease had lost
  // priority and moved to the tail.
  const auto trade_events = h.new_order(make_limit(Side::kBuy, 1000, 50, 20, 1));
  bool traded_against_original = false;
  for (const auto& event : trade_events) {
    if (event.message_type == MessageType::kTrade) {
      const auto trade = aegis::events::exchange::decode_trade(event.payload);
      traded_against_original =
          trade.value_or(aegis::events::exchange::TradeEvent{.maker_order_id = 0}).maker_order_id ==
          1;
    }
  }
  EXPECT_TRUE(traded_against_original);
}

TEST(ModifySemantics, QuantityDecreaseOriginalQuantityStaysFixed) {
  Harness h;
  h.new_order(make_limit(Side::kSell, 1000, 100, 10, 1));
  h.modify(ModifyOrderCommand{.instrument_id = 1,
                              .participant_id = 10,
                              .order_id = 1,
                              .new_price_units = 1000,
                              .new_quantity_units = 50});

  const auto* order = h.book.find(OrderId{1});
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->original_quantity, QuantityUnits{100}) << "P1: original_quantity never changes";
  EXPECT_EQ(order->remaining, QuantityUnits{50});
  EXPECT_EQ(order->cancelled_quantity, QuantityUnits{50});
}

TEST(ModifySemantics, ModifyBelowCumulativeFilledIsRejected) {
  Harness h;
  h.new_order(make_limit(Side::kSell, 1000, 150, 10, 1));  // OrderId 1
  h.new_order(make_limit(Side::kBuy, 1000, 100, 20, 1));   // fills 100 of OrderId 1

  const auto events = h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                  .participant_id = 10,
                                                  .order_id = 1,
                                                  .new_price_units = 1000,
                                                  .new_quantity_units = 50});  // < 100 filled
  ASSERT_EQ(events.size(), 1U);
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kModifyBelowFilled);

  const auto* order = h.book.find(OrderId{1});
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->remaining, QuantityUnits{50}) << "a rejected modify must not touch the order";
}

// ---------------------------------------------------------- cancel-replace

// AEGIS-031/032: a quantity increase is cancel-replace — new OrderId, new
// priority at the tail, old priority lost.
TEST(ModifySemantics, QuantityIncreaseIsCancelReplaceWithANewOrderIdAtTheTail) {
  Harness h;
  h.new_order(make_limit(Side::kSell, 1000, 50, 10, 1));  // OrderId 1: increased below
  h.new_order(make_limit(Side::kSell, 1000, 50, 11, 2));  // OrderId 2: arrived after

  const auto events = h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                  .participant_id = 10,
                                                  .order_id = 1,
                                                  .new_price_units = 1000,
                                                  .new_quantity_units = 100});
  // Terminated(kReplaced) for OrderId 1, Replaced{old=1,new=3}, Accepted-
  // equivalent is folded into OrderReplaced (no separate OrderAccepted).
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(decode_order_terminated(events[0].payload),
            (OrderTerminatedEvent{.causing_command_sequence = 3,
                                  .order_id = 1,
                                  .reason = TerminationReason::kReplaced,
                                  .cancelled_quantity_delta_units = 50}));
  EXPECT_EQ(decode_order_replaced(events[1].payload),
            (OrderReplacedEvent{.causing_command_sequence = 3,
                                .old_order_id = 1,
                                .new_order_id = 3,
                                .instrument_id = 1,
                                .participant_id = 10,
                                .client_order_id = 1,
                                .side = Side::kSell,
                                .order_type = OrderType::kLimit,
                                .price_units = 1000,
                                .quantity_units = 100}));

  EXPECT_EQ(h.book.find(OrderId{1}), nullptr) << "the old order is gone";
  const auto ids = h.book.orders_at(Side::kSell, PriceUnits{1000});
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids[0], OrderId{2}) << "the earlier order keeps its position";
  EXPECT_EQ(ids[1], OrderId{3}) << "the replacement goes to the tail, losing its old priority";
}

TEST(ModifySemantics, PriceChangeIsCancelReplaceEvenWithAnUnchangedQuantity) {
  Harness h;
  h.new_order(make_limit(Side::kSell, 1000, 50, 10, 1));  // OrderId 1

  const auto events = h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                  .participant_id = 10,
                                                  .order_id = 1,
                                                  .new_price_units = 1025,
                                                  .new_quantity_units = 50});
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderTerminated);
  EXPECT_EQ(events[1].message_type, MessageType::kOrderReplaced);

  EXPECT_EQ(h.book.find(OrderId{1}), nullptr);
  EXPECT_EQ(h.book.best_ask(), PriceUnits{1025});
}

TEST(ModifySemantics, ReplacementThatNowCrossesTradesImmediately) {
  Harness h;
  h.new_order(make_limit(Side::kBuy, 1000, 50, 30, 1));   // resting bid
  h.new_order(make_limit(Side::kSell, 1500, 50, 10, 1));  // OrderId 2: far from the bid

  const auto events = h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                  .participant_id = 10,
                                                  .order_id = 2,
                                                  .new_price_units = 1000,  // now crosses the bid
                                                  .new_quantity_units = 50});
  bool traded = false;
  bool taker_terminated_filled = false;
  for (const auto& event : events) {
    if (event.message_type == MessageType::kTrade) {
      traded = true;
    }
    if (event.message_type == MessageType::kOrderTerminated) {
      const auto terminated = decode_order_terminated(event.payload);
      if (terminated == (OrderTerminatedEvent{.causing_command_sequence = 3,
                                              .order_id = 3,
                                              .reason = TerminationReason::kFilled,
                                              .cancelled_quantity_delta_units = 0})) {
        taker_terminated_filled = true;
      }
    }
  }
  EXPECT_TRUE(traded) << "a replacement that now crosses must match immediately";
  EXPECT_TRUE(taker_terminated_filled);
  EXPECT_EQ(h.book.live_order_count(), 0U);
}

TEST(ModifySemantics, ModifyOfUnknownOrderIsRejected) {
  Harness h;
  const auto events = h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                  .participant_id = 10,
                                                  .order_id = 999,
                                                  .new_price_units = 1000,
                                                  .new_quantity_units = 50});
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kUnknownOrderId);
}

TEST(ModifySemantics, ModifyByAnyoneOtherThanTheOwnerIsRejected) {
  Harness h;
  h.new_order(make_limit(Side::kBuy, 1000, 100, 10, 1));  // OrderId 1

  const auto events = h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                  .participant_id = 999,
                                                  .order_id = 1,
                                                  .new_price_units = 1000,
                                                  .new_quantity_units = 50});
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kNotOrderOwner);
  const auto* order = h.book.find(OrderId{1});
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->remaining, QuantityUnits{100}) << "a rejected modify must not touch the order";
}

TEST(ModifySemantics, ModifyToAnOffTickPriceIsRejected) {
  Harness h;
  h.new_order(make_limit(Side::kBuy, 1000, 100, 10, 1));

  const auto events = h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                  .participant_id = 10,
                                                  .order_id = 1,
                                                  .new_price_units = 1010,
                                                  .new_quantity_units = 100});
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kPriceNotOnTick);
}

TEST(ModifySemantics, ModifyToAnOffLotQuantityIsRejected) {
  Harness h;
  h.new_order(make_limit(Side::kBuy, 1000, 100, 10, 1));

  const auto events = h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                  .participant_id = 10,
                                                  .order_id = 1,
                                                  .new_price_units = 1000,
                                                  .new_quantity_units = 130});
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kQuantityNotOnLot);
}

}  // namespace
