#include <cstdint>
#include <random>

#include <gtest/gtest.h>

#include "cpp/exchange/app/exchange_node.hpp"
#include "cpp/exchange/order_book/invariants.hpp"

/// AEGIS-041: a seeded random command stream, checked with
/// `InvariantScope::kQuiescent` after every command — the boundary at which
/// it is meaningful, since an aggressor is legitimately crossed with the
/// book for the duration of matching itself (ADR-0010).
namespace {

using aegis::common::EventTime;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::exchange::check_invariants;
using aegis::exchange::ExchangeNode;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::InvariantScope;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;

InstrumentSpec make_spec() {
  return InstrumentSpec{
      .instrument_id = InstrumentId{1},
      .price_floor_units = PriceUnits{1000},
      .price_ceiling_units = PriceUnits{2000},
      .tick_size_units = 25,
      .min_quantity_units = QuantityUnits{50},
      .max_quantity_units = QuantityUnits{5000},
      .lot_size_units = 50,
  };
}

TEST(BookInvariantsFuzz, QuiescentHoldsAfterEveryGeneratedCommand) {
  // A fixed seed is deliberate (AEGIS-005): reproducibility is the point.
  std::mt19937 rng{2026};  // NOLINT(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::uniform_int_distribution<int> command_choice(0, 2);  // 0=new, 1=cancel, 2=modify
  std::uniform_int_distribution<int> side_choice(0, 1);
  std::uniform_int_distribution<int> order_type_choice(0, 9);  // mostly limit, some market
  std::uniform_int_distribution<std::int64_t> price_lot_dist(0, 40);
  std::uniform_int_distribution<std::int64_t> quantity_lot_dist(1, 20);
  std::uniform_int_distribution<std::uint64_t> participant_dist(1, 6);
  std::uniform_int_distribution<std::uint64_t> target_order_dist(1, 400);

  ExchangeNode node;
  node.add_instrument(make_spec());

  constexpr int kCommands = 1500;
  std::uint64_t next_client_order_id = 1;

  for (int i = 0; i < kCommands; ++i) {
    const auto command_sequence = node.sequencer().sequence(EventTime{i});
    const auto participant = participant_dist(rng);

    switch (command_choice(rng)) {
      case 0: {
        const bool is_market = order_type_choice(rng) == 0;
        std::ignore = node.apply_new_order(
            NewOrderCommand{
                .instrument_id = 1,
                .participant_id = participant,
                .client_order_id = next_client_order_id++,
                .side = side_choice(rng) == 0 ? Side::kBuy : Side::kSell,
                .order_type = is_market ? OrderType::kMarket : OrderType::kLimit,
                .price_units = is_market ? 0 : 1000 + (price_lot_dist(rng) * 25),
                .quantity_units = quantity_lot_dist(rng) * 50,
            },
            command_sequence);
        break;
      }
      case 1: {
        std::ignore =
            node.apply_cancel_order(CancelOrderCommand{.instrument_id = 1,
                                                       .participant_id = participant,
                                                       .order_id = target_order_dist(rng)},
                                    command_sequence);
        break;
      }
      default: {
        std::ignore = node.apply_modify_order(
            ModifyOrderCommand{.instrument_id = 1,
                               .participant_id = participant,
                               .order_id = target_order_dist(rng),
                               .new_price_units = 1000 + (price_lot_dist(rng) * 25),
                               .new_quantity_units = quantity_lot_dist(rng) * 50},
            command_sequence);
        break;
      }
    }

    const auto* book = node.book(InstrumentId{1});
    ASSERT_NE(book, nullptr);
    const auto violations = check_invariants(*book, InvariantScope::kQuiescent);
    ASSERT_TRUE(violations.empty()) << "after command " << i << ": " << violations.front();
  }
}

}  // namespace
