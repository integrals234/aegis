#include <optional>
#include <tuple>

#include <gtest/gtest.h>

#include "cpp/exchange/matching/fifo_policy.hpp"
#include "cpp/exchange/order_book/book.hpp"

/// AEGIS-027, AEGIS-039, AEGIS-040: `FifoPolicy` is a pure decision — it never
/// mutates the book — so these tests build a book directly and check the
/// pairings it proposes. The full accept/trade/terminate orchestration lands
/// with `MatchingEngine` (slice 2); this is the golden-sequence proof that the
/// decision itself is price-then-time correct.
namespace {

using aegis::exchange::ClientOrderId;
using aegis::exchange::CommandSequence;
using aegis::exchange::FifoPolicy;
using aegis::exchange::InstrumentId;
using aegis::exchange::OrderBook;
using aegis::exchange::OrderId;
using aegis::exchange::OrderNode;
using aegis::exchange::OrderType;
using aegis::exchange::Pairing;
using aegis::exchange::ParticipantId;
using aegis::exchange::PriceUnits;
using aegis::exchange::Priority;
using aegis::exchange::QuantityUnits;
using aegis::exchange::Side;

OrderNode make_resting(std::uint64_t id, Side side, PriceUnits price, QuantityUnits quantity,
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

TEST(FifoPolicy, PriceBeatsArrivalAcrossLevels) {
  OrderBook book{InstrumentId{1}};
  // Two resting asks: a worse-priced order arrived first, a better-priced
  // order arrived second. A buy aggressor must still take the better price.
  book.add(make_resting(1, Side::kSell, PriceUnits{105}, QuantityUnits{10}, 1));
  book.add(make_resting(2, Side::kSell, PriceUnits{100}, QuantityUnits{10}, 2));

  const FifoPolicy policy;
  const auto pairings = policy.match(book, Side::kBuy, PriceUnits{110}, QuantityUnits{10});

  ASSERT_EQ(pairings.size(), 1U);
  EXPECT_EQ(pairings[0],
            (Pairing{.maker_order_id = OrderId{2}, .fill_quantity = QuantityUnits{10}}));
}

TEST(FifoPolicy, ArrivalOrderBreaksTiesAtTheSamePrice) {
  OrderBook book{InstrumentId{1}};
  book.add(make_resting(1, Side::kSell, PriceUnits{100}, QuantityUnits{5}, 1));
  book.add(make_resting(2, Side::kSell, PriceUnits{100}, QuantityUnits{5}, 2));
  book.add(make_resting(3, Side::kSell, PriceUnits{100}, QuantityUnits{5}, 3));

  const FifoPolicy policy;
  const auto pairings = policy.match(book, Side::kBuy, PriceUnits{100}, QuantityUnits{12});

  ASSERT_EQ(pairings.size(), 3U);
  EXPECT_EQ(pairings[0],
            (Pairing{.maker_order_id = OrderId{1}, .fill_quantity = QuantityUnits{5}}));
  EXPECT_EQ(pairings[1],
            (Pairing{.maker_order_id = OrderId{2}, .fill_quantity = QuantityUnits{5}}));
  EXPECT_EQ(pairings[2],
            (Pairing{.maker_order_id = OrderId{3},
                     .fill_quantity = QuantityUnits{2}}));  // partial: quantity exhausted
}

TEST(FifoPolicy, TwoCommandsSharingAnEventTimeStillOrderByCommandSequence) {
  // Both resting orders are constructed with the same conceptual arrival
  // instant; only CommandSequence (via Priority) distinguishes them. FIFO
  // order must follow CommandSequence, never wall-clock resolution.
  OrderBook book{InstrumentId{1}};
  book.add(make_resting(1, Side::kSell, PriceUnits{100}, QuantityUnits{5}, 10));
  book.add(make_resting(2, Side::kSell, PriceUnits{100}, QuantityUnits{5}, 11));

  const FifoPolicy policy;
  const auto pairings = policy.match(book, Side::kBuy, PriceUnits{100}, QuantityUnits{5});

  ASSERT_EQ(pairings.size(), 1U);
  EXPECT_EQ(pairings[0].maker_order_id, OrderId{1}) << "CommandSequence 10 was assigned first";
}

TEST(FifoPolicy, DoesNotCrossBeyondTheAggressorsLimit) {
  OrderBook book{InstrumentId{1}};
  book.add(make_resting(1, Side::kSell, PriceUnits{100}, QuantityUnits{5}, 1));
  book.add(make_resting(2, Side::kSell, PriceUnits{110}, QuantityUnits{5}, 2));

  const FifoPolicy policy;
  const auto pairings = policy.match(book, Side::kBuy, PriceUnits{105}, QuantityUnits{100});

  ASSERT_EQ(pairings.size(), 1U);
  EXPECT_EQ(pairings[0].maker_order_id, OrderId{1});
}

TEST(FifoPolicy, MarketAggressorHasNoLimitAndSweepsMultipleLevels) {
  OrderBook book{InstrumentId{1}};
  book.add(make_resting(1, Side::kSell, PriceUnits{100}, QuantityUnits{5}, 1));
  book.add(make_resting(2, Side::kSell, PriceUnits{110}, QuantityUnits{5}, 2));
  book.add(make_resting(3, Side::kSell, PriceUnits{120}, QuantityUnits{5}, 3));

  const FifoPolicy policy;
  const auto pairings = policy.match(book, Side::kBuy, std::nullopt, QuantityUnits{12});

  ASSERT_EQ(pairings.size(), 3U);
  EXPECT_EQ(pairings[0],
            (Pairing{.maker_order_id = OrderId{1}, .fill_quantity = QuantityUnits{5}}));
  EXPECT_EQ(pairings[1],
            (Pairing{.maker_order_id = OrderId{2}, .fill_quantity = QuantityUnits{5}}));
  EXPECT_EQ(pairings[2],
            (Pairing{.maker_order_id = OrderId{3}, .fill_quantity = QuantityUnits{2}}));
}

TEST(FifoPolicy, EmptyBookProducesNoPairings) {
  OrderBook book{InstrumentId{1}};
  const FifoPolicy policy;
  const auto pairings = policy.match(book, Side::kBuy, std::nullopt, QuantityUnits{10});
  EXPECT_TRUE(pairings.empty());
}

TEST(FifoPolicy, DoesNotMutateTheBook) {
  OrderBook book{InstrumentId{1}};
  book.add(make_resting(1, Side::kSell, PriceUnits{100}, QuantityUnits{5}, 1));

  const FifoPolicy policy;
  std::ignore = policy.match(book, Side::kBuy, PriceUnits{100}, QuantityUnits{5});

  const auto* order = book.find(OrderId{1});
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->remaining, QuantityUnits{5}) << "match() decides; it must never apply";
}

}  // namespace
