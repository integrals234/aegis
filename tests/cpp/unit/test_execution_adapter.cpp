#include <vector>

#include <gtest/gtest.h>

#include "cpp/common/clock.hpp"
#include "cpp/participant/oms/transport_execution_adapter.hpp"

/// AEGIS-119: `TransportExecutionAdapter` encodes OMS intent to the exact
/// wire form `cpp/events/exchange_messages.hpp` already defines and hands it
/// to an injected `ExecutionTransport`. A fake in-test transport is the only
/// place a concrete `ExecutionTransport` exists outside production code —
/// production ships no transport implementation at M3 (ADR-0023).
namespace {

using aegis::common::ManualClock;
using aegis::events::Envelope;
using aegis::events::MessageType;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::decode_cancel_order;
using aegis::events::exchange::decode_modify_order;
using aegis::events::exchange::decode_new_order;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::participant::oms::ExecutionTransport;
using aegis::participant::oms::TransportExecutionAdapter;

class RecordingTransport final : public ExecutionTransport {
 public:
  [[nodiscard]] bool send(const Envelope& envelope) override {
    sent.push_back(envelope);
    return accept_next;
  }

  std::vector<Envelope> sent;
  bool accept_next{true};
};

TEST(TransportExecutionAdapter, SubmitEncodesAndSendsANewOrderCommand) {
  RecordingTransport transport;
  ManualClock clock{1'000};
  TransportExecutionAdapter adapter(transport, clock, /*stream_id=*/1);

  const NewOrderCommand command{.instrument_id = 1,
                                .participant_id = 2,
                                .client_order_id = 3,
                                .side = Side::kBuy,
                                .order_type = OrderType::kLimit,
                                .price_units = 100,
                                .quantity_units = 10};
  EXPECT_TRUE(adapter.submit(command));

  ASSERT_EQ(transport.sent.size(), 1U);
  EXPECT_EQ(transport.sent[0].message_type, MessageType::kNewOrder);
  EXPECT_EQ(transport.sent[0].stream_id, 1U);
  const auto decoded = decode_new_order(transport.sent[0].payload);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value_or({}), command);
}

TEST(TransportExecutionAdapter, CancelEncodesAndSends) {
  RecordingTransport transport;
  ManualClock clock;
  TransportExecutionAdapter adapter(transport, clock, /*stream_id=*/1);

  const CancelOrderCommand command{.instrument_id = 1, .participant_id = 2, .order_id = 7};
  EXPECT_TRUE(adapter.cancel(command));

  ASSERT_EQ(transport.sent.size(), 1U);
  EXPECT_EQ(transport.sent[0].message_type, MessageType::kCancelOrder);
  EXPECT_EQ(decode_cancel_order(transport.sent[0].payload), command);
}

TEST(TransportExecutionAdapter, ModifyEncodesAndSends) {
  RecordingTransport transport;
  ManualClock clock;
  TransportExecutionAdapter adapter(transport, clock, /*stream_id=*/1);

  const ModifyOrderCommand command{.instrument_id = 1,
                                   .participant_id = 2,
                                   .order_id = 7,
                                   .new_price_units = 105,
                                   .new_quantity_units = 20};
  EXPECT_TRUE(adapter.modify(command));

  ASSERT_EQ(transport.sent.size(), 1U);
  EXPECT_EQ(transport.sent[0].message_type, MessageType::kModifyOrder);
  EXPECT_EQ(decode_modify_order(transport.sent[0].payload), command);
}

TEST(TransportExecutionAdapter, SequenceIncrementsAcrossCalls) {
  RecordingTransport transport;
  ManualClock clock;
  TransportExecutionAdapter adapter(transport, clock, /*stream_id=*/1);

  EXPECT_TRUE(adapter.submit(NewOrderCommand{}));
  EXPECT_TRUE(adapter.cancel(CancelOrderCommand{}));
  ASSERT_EQ(transport.sent.size(), 2U);
  EXPECT_LT(transport.sent[0].sequence, transport.sent[1].sequence);
}

TEST(TransportExecutionAdapter, TransportRefusalPropagates) {
  RecordingTransport transport;
  transport.accept_next = false;
  ManualClock clock;
  TransportExecutionAdapter adapter(transport, clock, /*stream_id=*/1);

  EXPECT_FALSE(adapter.submit(NewOrderCommand{}));
}

}  // namespace
