#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "cpp/exchange/order_book/invariants.hpp"

/// AEGIS-041: the debug-only invariant checker, both scopes. Deliberately
/// corrupted fixtures are built by calling `OrderBook::add` directly —
/// bypassing `MatchingEngine`, which is what normally keeps these
/// invariants true — so a checker that always passes is caught.
namespace {

using aegis::exchange::check_invariants;
using aegis::exchange::ClientOrderId;
using aegis::exchange::CommandSequence;
using aegis::exchange::InstrumentId;
using aegis::exchange::InvariantScope;
using aegis::exchange::OrderBook;
using aegis::exchange::OrderId;
using aegis::exchange::OrderNode;
using aegis::exchange::OrderType;
using aegis::exchange::ParticipantId;
using aegis::exchange::PriceUnits;
using aegis::exchange::Priority;
using aegis::exchange::QuantityUnits;
using aegis::exchange::Side;

OrderNode make_order(std::uint64_t id, Side side, PriceUnits price, QuantityUnits original,
                     QuantityUnits remaining, std::uint64_t command_sequence,
                     QuantityUnits cumulative_filled = QuantityUnits{0},
                     QuantityUnits cancelled_quantity = QuantityUnits{0}) {
  OrderNode node;
  node.order_id = OrderId{id};
  node.instrument_id = InstrumentId{1};
  node.participant_id = ParticipantId{1};
  node.client_order_id = ClientOrderId{id};
  node.side = side;
  node.order_type = OrderType::kLimit;
  node.price_units = price;
  node.original_quantity = original;
  node.cumulative_filled = cumulative_filled;
  node.cancelled_quantity = cancelled_quantity;
  node.remaining = remaining;
  node.priority = Priority::from(CommandSequence{command_sequence});
  return node;
}

TEST(BookInvariants, CleanBookPassesBothScopes) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{1000}, QuantityUnits{100}, QuantityUnits{100}, 1));
  book.add(make_order(2, Side::kSell, PriceUnits{1100}, QuantityUnits{100}, QuantityUnits{100}, 2));

  EXPECT_TRUE(check_invariants(book, InvariantScope::kStructural).empty());
  EXPECT_TRUE(check_invariants(book, InvariantScope::kQuiescent).empty());
}

TEST(BookInvariants, EmptyBookPassesBothScopes) {
  OrderBook book{InstrumentId{1}};
  EXPECT_TRUE(check_invariants(book, InvariantScope::kStructural).empty());
  EXPECT_TRUE(check_invariants(book, InvariantScope::kQuiescent).empty());
}

// AEGIS-041: an aggressor is legitimately crossed with the book mid-match —
// kStructural must not fire on a crossed book; only kQuiescent checks that.
TEST(BookInvariants, CrossedBookPassesStructuralButFailsQuiescent) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{1100}, QuantityUnits{100}, QuantityUnits{100}, 1));
  book.add(make_order(2, Side::kSell, PriceUnits{1000}, QuantityUnits{100}, QuantityUnits{100}, 2));

  EXPECT_TRUE(check_invariants(book, InvariantScope::kStructural).empty())
      << "a crossed book is not a structural violation";

  const auto violations = check_invariants(book, InvariantScope::kQuiescent);
  EXPECT_FALSE(violations.empty());
  bool found_crossed = false;
  for (const auto& violation : violations) {
    found_crossed = found_crossed || violation.find("crossed") != std::string::npos;
  }
  EXPECT_TRUE(found_crossed);
}

TEST(BookInvariants, RemainingAboveOriginalQuantityFailsStructural) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{1000}, QuantityUnits{100}, QuantityUnits{150}, 1));

  const auto violations = check_invariants(book, InvariantScope::kStructural);
  EXPECT_FALSE(violations.empty());
}

TEST(BookInvariants, ZeroRemainingFailsStructural) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{1000}, QuantityUnits{100}, QuantityUnits{0}, 1));

  const auto violations = check_invariants(book, InvariantScope::kStructural);
  EXPECT_FALSE(violations.empty());
}

TEST(BookInvariants, PriorityNotIncreasingAlongQueueFailsStructural) {
  OrderBook book{InstrumentId{1}};
  // Inserted in reverse priority order: add() always appends to the tail
  // regardless of the descriptor's own priority value, so this produces a
  // queue whose priority decreases from head to tail.
  book.add(make_order(1, Side::kBuy, PriceUnits{1000}, QuantityUnits{100}, QuantityUnits{100},
                      /*command_sequence=*/10));
  book.add(make_order(2, Side::kBuy, PriceUnits{1000}, QuantityUnits{100}, QuantityUnits{100},
                      /*command_sequence=*/5));

  const auto violations = check_invariants(book, InvariantScope::kStructural);
  EXPECT_FALSE(violations.empty());
}

// P1 (ADR-0011 §4.10) is a kQuiescent-only check: it is meaningful once a
// command has fully applied, not as a mid-match structural property.
TEST(BookInvariants, P1ViolationFailsOnlyQuiescent) {
  OrderBook book{InstrumentId{1}};
  // cumulative_filled(30) + remaining(50) + cancelled_quantity(10) = 90, but
  // original_quantity claims 100 — P1 does not hold.
  book.add(make_order(1, Side::kBuy, PriceUnits{1000}, QuantityUnits{100}, QuantityUnits{50},
                      /*command_sequence=*/1, /*cumulative_filled=*/QuantityUnits{30},
                      /*cancelled_quantity=*/QuantityUnits{10}));

  EXPECT_TRUE(check_invariants(book, InvariantScope::kStructural).empty())
      << "P1 is a quiescent-only invariant";

  const auto violations = check_invariants(book, InvariantScope::kQuiescent);
  EXPECT_FALSE(violations.empty());
  bool found_p1 = false;
  for (const auto& violation : violations) {
    found_p1 = found_p1 || violation.find("P1") != std::string::npos;
  }
  EXPECT_TRUE(found_p1);
}

TEST(BookInvariants, CancelRestoresBothScopesToClean) {
  OrderBook book{InstrumentId{1}};
  book.add(make_order(1, Side::kBuy, PriceUnits{1000}, QuantityUnits{100}, QuantityUnits{100}, 1));
  book.cancel(OrderId{1});

  EXPECT_TRUE(check_invariants(book, InvariantScope::kStructural).empty());
  EXPECT_TRUE(check_invariants(book, InvariantScope::kQuiescent).empty());
}

}  // namespace
