#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/exchange/order_book/book.hpp"

/// AEGIS-036: lookup/cancel correctness under insert/erase churn. A seeded
/// PRNG drives random add/cancel operations against `OrderBook` while a
/// plain `std::unordered_map` reference model tracks which order ids ought
/// to be live; after every operation, `OrderBook::find` is checked against
/// the reference for both live and already-removed ids.
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

OrderNode make_order(std::uint64_t id, Side side, PriceUnits price) {
  OrderNode node;
  node.order_id = OrderId{id};
  node.instrument_id = InstrumentId{1};
  node.participant_id = ParticipantId{id};
  node.client_order_id = ClientOrderId{id};
  node.side = side;
  node.order_type = OrderType::kLimit;
  node.price_units = price;
  node.original_quantity = QuantityUnits{100};
  node.remaining = QuantityUnits{100};
  node.priority = Priority::from(CommandSequence{id});
  return node;
}

TEST(OrderIndex, FindAndCancelStayCorrectUnderRandomizedChurn) {
  constexpr int kOperations = 2000;
  constexpr int kPricePoints = 5;

  // A fixed seed is deliberate (AEGIS-005): this test's whole point is a
  // reproducible generated sequence, not cryptographic unpredictability —
  // the failure this check guards against does not apply here.
  std::mt19937 rng{42};  // NOLINT(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::uniform_int_distribution<int> op_choice(0, 1);  // 0 = add, 1 = cancel
  std::uniform_int_distribution<int> price_choice(0, kPricePoints - 1);
  std::uniform_int_distribution<int> side_choice(0, 1);

  OrderBook book{InstrumentId{1}};
  book.reserve(kOperations);
  std::unordered_set<std::uint64_t> live_ids;
  std::uint64_t next_id = 1;

  for (int i = 0; i < kOperations; ++i) {
    const bool should_add = live_ids.empty() || op_choice(rng) == 0;
    if (should_add) {
      const auto id = next_id++;
      const auto side = side_choice(rng) == 0 ? Side::kBuy : Side::kSell;
      const PriceUnits price{1000 + (price_choice(rng) * 25)};
      book.add(make_order(id, side, price));
      live_ids.insert(id);
    } else {
      // Pick an arbitrary live id to cancel (unordered_set iteration order is
      // unspecified but deterministic for a fixed sequence of insertions on a
      // fixed implementation — good enough for a seeded, single-run churn
      // test; this is not asserting anything about iteration order itself).
      const auto id = *live_ids.begin();
      const auto removed = book.cancel(OrderId{id});
      EXPECT_TRUE(removed.has_value()) << "id " << id << " was live in the reference model";
      live_ids.erase(id);
    }

    // Every id ever assigned is either live (found) or removed (not found) —
    // checked against the reference model after every single operation.
    for (std::uint64_t id = 1; id < next_id; ++id) {
      const bool is_live = live_ids.contains(id);
      const auto* found = book.find(OrderId{id});
      EXPECT_EQ(found != nullptr, is_live) << "id " << id << " at operation " << i;
    }
    EXPECT_EQ(book.live_order_count(), live_ids.size());
  }
}

}  // namespace
