#include <gtest/gtest.h>

#include "cpp/exchange/app/exchange_node.hpp"

/// AEGIS-064/065, ADR-0020: MarketDataPublisher derives correct deltas from
/// a cancel-replace (a price change forces a new OrderId, per ADR-0011),
/// which ExchangeNode's own test file does not exercise.
namespace {

using aegis::events::CommandSequence;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::events::market_data::DeltaKind;
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

TEST(MarketDataPublisher, PriceChangeModifyProducesRemoveThenAddDeltas) {
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
  static_cast<void>(node.derive_market_data(accepted));

  // A price change forces a cancel-replace (ADR-0011): a new order_id at the
  // new price, and the old order terminates.
  const auto replaced = node.apply_modify_order(ModifyOrderCommand{.instrument_id = 1,
                                                                   .participant_id = 1,
                                                                   .order_id = 1,
                                                                   .new_price_units = 1025,
                                                                   .new_quantity_units = 100},
                                                CommandSequence{2});
  const auto deltas = node.derive_market_data(replaced);

  ASSERT_EQ(deltas.size(), 2U);
  EXPECT_EQ(deltas[0].kind, DeltaKind::kOrderRemoved);
  EXPECT_EQ(deltas[0].order_id, 1U);
  EXPECT_EQ(deltas[1].kind, DeltaKind::kOrderAdded);
  EXPECT_EQ(deltas[1].price_units, 1025);
}

TEST(MarketDataPublisher, QuantityDecreaseModifyProducesOneModifiedDelta) {
  ExchangeNode node;
  node.add_instrument(make_spec(1));
  // Original quantity 200, comfortably above the instrument's
  // min_quantity_units (100), so the decrease below stays in range.
  const auto accepted = node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                             .participant_id = 1,
                                                             .client_order_id = 1,
                                                             .side = Side::kBuy,
                                                             .order_type = OrderType::kLimit,
                                                             .price_units = 1000,
                                                             .quantity_units = 200},
                                             CommandSequence{1});
  static_cast<void>(node.derive_market_data(accepted));

  // A pure quantity decrease is an in-place modify (ADR-0011): same order_id.
  const auto modified = node.apply_modify_order(ModifyOrderCommand{.instrument_id = 1,
                                                                   .participant_id = 1,
                                                                   .order_id = 1,
                                                                   .new_price_units = 1000,
                                                                   .new_quantity_units = 150},
                                                CommandSequence{2});
  const auto deltas = node.derive_market_data(modified);

  ASSERT_EQ(deltas.size(), 1U);
  EXPECT_EQ(deltas[0].kind, DeltaKind::kOrderModified);
  EXPECT_EQ(deltas[0].order_id, 1U);
  EXPECT_EQ(deltas[0].quantity_units, 150);
}

}  // namespace
