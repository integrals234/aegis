#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "cpp/exchange/order_book/types.hpp"

namespace {

using aegis::exchange::ClientOrderId;
using aegis::exchange::CommandSequence;
using aegis::exchange::EventSequence;
using aegis::exchange::InstrumentId;
using aegis::exchange::OrderId;
using aegis::exchange::ParticipantId;
using aegis::exchange::PriceUnits;
using aegis::exchange::Priority;
using aegis::exchange::QuantityUnits;

TEST(ExchangeTypes, IdentifierSpacesAreDistinctTypes) {
  static_assert(!std::is_convertible_v<CommandSequence, EventSequence>);
  static_assert(!std::is_convertible_v<EventSequence, OrderId>);
  static_assert(!std::is_convertible_v<OrderId, CommandSequence>);
  static_assert(!std::is_convertible_v<InstrumentId, ParticipantId>);
  static_assert(!std::is_convertible_v<ParticipantId, ClientOrderId>);
  static_assert(!std::is_convertible_v<std::uint64_t, OrderId>);
  SUCCEED();
}

TEST(ExchangeTypes, IdentifiersCompareByValue) {
  EXPECT_EQ(OrderId{1}, OrderId{1});
  EXPECT_NE(OrderId{1}, OrderId{2});
  EXPECT_LT(CommandSequence{1}, CommandSequence{2});
}

TEST(ExchangeTypes, PriorityIsOnlyConstructibleFromCommandSequence) {
  static_assert(!std::is_constructible_v<Priority, std::uint64_t>);
  static_assert(!std::is_constructible_v<Priority, EventSequence>);

  const auto priority = Priority::from(CommandSequence{42});
  EXPECT_EQ(priority.command_sequence(), CommandSequence{42});
}

TEST(ExchangeTypes, PriorityOrdersByCommandSequence) {
  const auto earlier = Priority::from(CommandSequence{1});
  const auto later = Priority::from(CommandSequence{2});
  EXPECT_LT(earlier, later);
}

TEST(ExchangeTypes, QuantityUnitsSupportArithmeticUnlikeIdentifiers) {
  const QuantityUnits a{30};
  const QuantityUnits b{10};
  EXPECT_EQ(a - b, QuantityUnits{20});
  EXPECT_EQ(a + b, QuantityUnits{40});
  EXPECT_EQ(aegis::exchange::min(a, b), b);
}

TEST(ExchangeTypes, PriceUnitsOrderNumerically) {
  EXPECT_LT(PriceUnits{99}, PriceUnits{100});
  EXPECT_GT(PriceUnits{101}, PriceUnits{100});
}

}  // namespace
