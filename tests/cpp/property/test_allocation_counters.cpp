#include <cstdint>
#include <memory_resource>

#include <gtest/gtest.h>

#include "cpp/exchange/order_book/book.hpp"
#include "tests/cpp/support/counting_resource.hpp"

/// AEGIS-037: after construction and warmup within reserved capacity, a
/// steady-state add/cancel cycle performs zero allocations on the injected
/// resource.
///
/// `OrderStorage`'s slab is a `std::pmr::vector` with its own free list
/// (`cpp/exchange/order_book/order_storage.hpp`): once `reserve()`d, it never
/// calls its resource again as long as live orders stay within capacity, no
/// pool required. `order_index_` and `live_client_ids_` are node-based
/// (`std::pmr::unordered_map`/`std::pmr::map`): a `reserve()` there only sizes
/// the bucket array, never the per-node storage, so *any* upstream resource
/// sees an allocate/deallocate call on every insert/erase — that is a
/// property of node-based containers, not a bug in this book. A
/// `std::pmr::unsynchronized_pool_resource` sitting between the book and the
/// counted resource absorbs same-sized node churn after warmup populates its
/// free lists, which is the standard way to make "steady state is
/// allocation-free" a testable claim for containers shaped this way.
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
using aegis::exchange::testing::CountingResource;

constexpr int kLevels = 4;
constexpr int kOrdersPerLevel = 4;
constexpr int kWarmedOrders = kLevels * kOrdersPerLevel;
constexpr std::size_t kCapacity = 256;

OrderNode make_order(std::uint64_t id, PriceUnits price, std::uint64_t command_sequence) {
  OrderNode node;
  node.order_id = OrderId{id};
  node.instrument_id = InstrumentId{1};
  node.participant_id = ParticipantId{1};
  node.client_order_id = ClientOrderId{id};
  node.side = Side::kBuy;
  node.order_type = OrderType::kLimit;
  node.price_units = price;
  node.original_quantity = QuantityUnits{100};
  node.remaining = QuantityUnits{100};
  node.priority = Priority::from(CommandSequence{command_sequence});
  return node;
}

/// One add/cancel churn cycle: cancels the oldest order at `level` and adds a
/// fresh one at the tail, keeping that level's order_count constant — never
/// zero, so `LevelIndex::erase` is never called, and never touching a level
/// outside the warmed set, so no new price level is ever created.
void churn_cycle(OrderBook& book, int level, std::uint64_t& next_order_id) {
  const PriceUnits price{1000 + (level * 25)};
  const auto ids = book.orders_at(Side::kBuy, price);
  ASSERT_FALSE(ids.empty());
  book.cancel(ids.front());
  book.add(make_order(next_order_id, price, next_order_id));
  ++next_order_id;
}

TEST(AllocationCounters, SteadyStateAddCancelCycleAllocatesNothing) {
  CountingResource counted;
  std::pmr::unsynchronized_pool_resource pool{&counted};
  OrderBook book{InstrumentId{1}, &pool};
  book.reserve(kCapacity);

  std::uint64_t next_order_id = 1;
  for (int level = 0; level < kLevels; ++level) {
    const PriceUnits price{1000 + (level * 25)};
    for (int i = 0; i < kOrdersPerLevel; ++i) {
      book.add(make_order(next_order_id, price, next_order_id));
      ++next_order_id;
    }
  }
  ASSERT_EQ(book.live_order_count(), static_cast<std::size_t>(kWarmedOrders));

  // Warmup: run the exact steady-state pattern once so the pool's per-size
  // free lists are populated before counting starts.
  constexpr int kCycles = 64;
  for (int i = 0; i < kCycles; ++i) {
    churn_cycle(book, i % kLevels, next_order_id);
  }
  ASSERT_EQ(book.live_order_count(), static_cast<std::size_t>(kWarmedOrders))
      << "churn must not change how many orders are resting";

  counted.reset_counts();

  for (int i = 0; i < kCycles; ++i) {
    churn_cycle(book, i % kLevels, next_order_id);
  }

  EXPECT_EQ(counted.allocate_calls(), 0U);
  EXPECT_EQ(counted.deallocate_calls(), 0U);
  EXPECT_EQ(book.live_order_count(), static_cast<std::size_t>(kWarmedOrders));
}

}  // namespace
