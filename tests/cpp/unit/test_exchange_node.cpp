#include <gtest/gtest.h>

#include "cpp/exchange/app/exchange_node.hpp"

namespace {

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

}  // namespace
