#include <gtest/gtest.h>

#include "cpp/participant/oms/recorded_response_adapter.hpp"
#include "tests/cpp/optional_access.hpp"

/// AEGIS-112, AEGIS-119: RecordedResponseAdapter drains a committed script
/// deterministically, one response per call, in order.
namespace {

using aegis::events::MessageType;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::participant::oms::RecordedResponseAdapter;
using aegis::participant::oms::ScriptedResponse;

TEST(RecordedResponseAdapter, EveryCallSucceedsAndAdvancesTheCursor) {
  RecordedResponseAdapter adapter({});
  EXPECT_TRUE(adapter.submit(NewOrderCommand{}));
  EXPECT_TRUE(adapter.cancel(CancelOrderCommand{}));
  EXPECT_EQ(adapter.calls_made(), 2U);
}

TEST(RecordedResponseAdapter, NextResponseDrainsTheScriptInOrder) {
  RecordedResponseAdapter adapter(
      {ScriptedResponse{.message_type = MessageType::kOrderAccepted, .payload = {std::byte{1}}},
       ScriptedResponse{.message_type = MessageType::kOrderRejected, .payload = {std::byte{2}}}});

  EXPECT_TRUE(adapter.submit(NewOrderCommand{}));
  const auto first = adapter.next_response();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(aegis::test::checked(first).message_type, MessageType::kOrderAccepted);

  EXPECT_TRUE(adapter.submit(NewOrderCommand{}));
  const auto second = adapter.next_response();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(aegis::test::checked(second).message_type, MessageType::kOrderRejected);
}

TEST(RecordedResponseAdapter, NextResponseIsAConsumingPopNotAPeek) {
  RecordedResponseAdapter adapter(
      {ScriptedResponse{.message_type = MessageType::kOrderAccepted, .payload = {}}});
  EXPECT_TRUE(adapter.submit(NewOrderCommand{}));
  EXPECT_TRUE(adapter.next_response().has_value());
  EXPECT_FALSE(adapter.next_response().has_value());  // Already delivered once.
}

TEST(RecordedResponseAdapter, NoResponseAvailableBeforeACallOrPastTheScript) {
  RecordedResponseAdapter adapter({});
  EXPECT_FALSE(adapter.next_response().has_value());  // No call made yet.
  EXPECT_TRUE(adapter.submit(NewOrderCommand{}));
  EXPECT_FALSE(adapter.next_response().has_value());  // Script is empty.
}

}  // namespace
