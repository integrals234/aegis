#include <gtest/gtest.h>

#include "cpp/exchange/app/exchange_node.hpp"

namespace {

using aegis::events::CommandSequence;
using aegis::events::MessageType;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::decode_order_rejected;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderRejectedEvent;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::RejectReason;
using aegis::events::exchange::Side;
using aegis::exchange::ExchangeNode;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;

InstrumentSpec make_spec(std::uint32_t id) {
  InstrumentSpec spec;
  spec.instrument_id = InstrumentId{id};
  spec.price_floor_units = PriceUnits{1000};
  spec.price_ceiling_units = PriceUnits{2000};
  spec.tick_size_units = 25;
  spec.min_quantity_units = QuantityUnits{100};
  spec.max_quantity_units = QuantityUnits{10000};
  spec.lot_size_units = 50;
  return spec;
}

TEST(ExchangeNode, UnregisteredInstrumentHasNoBook) {
  ExchangeNode node;
  EXPECT_EQ(node.book(InstrumentId{1}), nullptr);
  EXPECT_EQ(node.instrument(InstrumentId{1}), nullptr);
}

TEST(ExchangeNode, AddInstrumentRegistersASpecAndAnEmptyBook) {
  ExchangeNode node;
  node.add_instrument(make_spec(1));

  const auto* spec = node.instrument(InstrumentId{1});
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(spec->tick_size_units, 25);

  auto* book = node.book(InstrumentId{1});
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->instrument_id(), InstrumentId{1});
  EXPECT_EQ(book->live_order_count(), 0U);
}

TEST(ExchangeNode, EachInstrumentGetsAnIndependentBook) {
  ExchangeNode node;
  node.add_instrument(make_spec(1));
  node.add_instrument(make_spec(2));

  EXPECT_NE(node.book(InstrumentId{1}), node.book(InstrumentId{2}));
}

TEST(ExchangeNode, SequencerAndEventLogAreSharedAcrossTheWholeNode) {
  ExchangeNode node;
  const auto first = node.sequencer().sequence(aegis::common::EventTime{0});
  const auto second = node.sequencer().sequence(aegis::common::EventTime{0});
  EXPECT_NE(first, second);
  EXPECT_EQ(node.event_log().next_event_sequence(), aegis::events::EventSequence{1});
}

// AEGIS-035: kUnknownInstrument never reaches MatchingEngine — ExchangeNode
// is the only layer that resolves instrument_id to a book, so an unknown one
// is rejected here, before an engine call is even possible.
TEST(ExchangeNode, NewOrderForAnUnregisteredInstrumentIsRejected) {
  ExchangeNode node;  // no instruments registered
  const auto events = node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                           .participant_id = 1,
                                                           .client_order_id = 1,
                                                           .side = Side::kBuy,
                                                           .order_type = OrderType::kLimit,
                                                           .price_units = 1000,
                                                           .quantity_units = 100},
                                           CommandSequence{1});
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderRejected);
  EXPECT_EQ(decode_order_rejected(events[0].payload),
            (OrderRejectedEvent{.causing_command_sequence = 1,
                                .instrument_id = 1,
                                .participant_id = 1,
                                .client_order_id = 1,
                                .order_id = 0,
                                .reason = RejectReason::kUnknownInstrument}));
}

TEST(ExchangeNode, CancelForAnUnregisteredInstrumentIsRejected) {
  ExchangeNode node;
  const auto events = node.apply_cancel_order(
      CancelOrderCommand{.instrument_id = 1, .participant_id = 1, .order_id = 7},
      CommandSequence{1});
  ASSERT_EQ(events.size(), 1U);
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kUnknownInstrument);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{.order_id = 0}).order_id, 7U);
}

TEST(ExchangeNode, ModifyForAnUnregisteredInstrumentIsRejected) {
  ExchangeNode node;
  const auto events = node.apply_modify_order(ModifyOrderCommand{.instrument_id = 1,
                                                                 .participant_id = 1,
                                                                 .order_id = 7,
                                                                 .new_price_units = 1000,
                                                                 .new_quantity_units = 100},
                                              CommandSequence{1});
  ASSERT_EQ(events.size(), 1U);
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kUnknownInstrument);
}

TEST(ExchangeNode, NewOrderForARegisteredInstrumentReachesTheEngine) {
  ExchangeNode node;
  node.add_instrument(make_spec(1));
  const auto events = node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                           .participant_id = 1,
                                                           .client_order_id = 1,
                                                           .side = Side::kBuy,
                                                           .order_type = OrderType::kLimit,
                                                           .price_units = 1000,
                                                           .quantity_units = 100},
                                           CommandSequence{1});
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderAccepted);
  EXPECT_EQ(node.book(InstrumentId{1})->live_order_count(), 1U);
}

// AEGIS-064/065, ADR-0020: the market-data half of the composition root is
// additive -- it observes apply_*'s own return value and the book it already
// maintains, and changes nothing about what apply_* returns.
TEST(ExchangeNode, MarketDataSnapshotReflectsAcceptedOrder) {
  ExchangeNode node;
  node.add_instrument(make_spec(1));
  const auto accepted_events = node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                                    .participant_id = 1,
                                                                    .client_order_id = 1,
                                                                    .side = Side::kBuy,
                                                                    .order_type = OrderType::kLimit,
                                                                    .price_units = 1000,
                                                                    .quantity_units = 100},
                                                    CommandSequence{1});
  ASSERT_EQ(accepted_events.size(), 1U);

  const auto snapshot = node.capture_market_data_snapshot(InstrumentId{1});
  ASSERT_EQ(snapshot.entries.size(), 1U);
  EXPECT_EQ(snapshot.entries[0].side, Side::kBuy);
  EXPECT_EQ(snapshot.entries[0].price_units, 1000);
  EXPECT_EQ(snapshot.entries[0].quantity_units, 100);
}

TEST(ExchangeNode, MarketDataDeltaReflectsAcceptedOrder) {
  ExchangeNode node;
  node.add_instrument(make_spec(1));
  const auto emitted = node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                            .participant_id = 1,
                                                            .client_order_id = 1,
                                                            .side = Side::kBuy,
                                                            .order_type = OrderType::kLimit,
                                                            .price_units = 1000,
                                                            .quantity_units = 100},
                                            CommandSequence{1});

  const auto deltas = node.derive_market_data(emitted);
  ASSERT_EQ(deltas.size(), 1U);
  EXPECT_EQ(deltas[0].kind, aegis::events::market_data::DeltaKind::kOrderAdded);
  EXPECT_EQ(deltas[0].price_units, 1000);
  EXPECT_EQ(deltas[0].quantity_units, 100);
}

TEST(ExchangeNode, MarketDataDeltaReflectsCancellation) {
  ExchangeNode node;
  node.add_instrument(make_spec(1));
  const auto accepted = node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                             .participant_id = 1,
                                                             .client_order_id = 1,
                                                             .side = Side::kBuy,
                                                             .order_type = OrderType::kLimit,
                                                             .price_units = 1000,
                                                             .quantity_units = 100},
                                             CommandSequence{1});
  static_cast<void>(node.derive_market_data(accepted));  // seed the publisher's order tracking

  const auto cancelled = node.apply_cancel_order(
      CancelOrderCommand{.instrument_id = 1, .participant_id = 1, .order_id = 1},
      CommandSequence{2});
  const auto deltas = node.derive_market_data(cancelled);
  ASSERT_EQ(deltas.size(), 1U);
  EXPECT_EQ(deltas[0].kind, aegis::events::market_data::DeltaKind::kOrderRemoved);
  EXPECT_EQ(deltas[0].quantity_units, 0);
}

}  // namespace
