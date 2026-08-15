#include <gtest/gtest.h>

#include "cpp/common/clock.hpp"
#include "cpp/exchange/app/exchange_node.hpp"
#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/oms/recorded_response_adapter.hpp"
#include "cpp/participant/oms/transport_execution_adapter.hpp"

/// AEGIS-119: "Contract tests pass across adapters." Each scenario below
/// runs the identical `OrderManager` call sequence twice -- once with
/// `TransportExecutionAdapter` fronting a real `ExchangeNode` (see
/// `test_participant_exchange_integration.cpp` for why that composition is
/// legal only in `tests/`), once with `RecordedResponseAdapter` scripted to
/// return an equivalently-shaped response encoded with the exact same
/// `cpp/events` `encode()` used by the real matching engine -- and asserts
/// `OrderManager` reaches the identical lifecycle state either way. That
/// equivalence, not any adapter-internal detail, is what
/// "environment-independent OMS" means.
///
/// Deliberately excluded: unsolicited trade/fill delivery.
/// `RecordedResponseAdapter` gates every scripted response behind a prior
/// submit/cancel/modify call (one call unlocks one response), so it cannot
/// script an event that arrives without a corresponding outbound call --
/// exactly like the real transport cannot be asked to invent one either.
/// Fill scenarios are proven against the real exchange in
/// `test_participant_exchange_integration.cpp` instead.
namespace {

using aegis::common::ManualClock;
using aegis::events::Envelope;
using aegis::events::MessageType;
using aegis::events::exchange::decode_cancel_order;
using aegis::events::exchange::decode_modify_order;
using aegis::events::exchange::decode_new_order;
using aegis::events::exchange::decode_order_accepted;
using aegis::events::exchange::decode_order_rejected;
using aegis::events::exchange::decode_order_terminated;
using aegis::events::exchange::encode;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderAcceptedEvent;
using aegis::events::exchange::OrderRejectedEvent;
using aegis::events::exchange::OrderTerminatedEvent;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::RejectReason;
using aegis::events::exchange::Side;
using aegis::events::exchange::TerminationReason;
using aegis::exchange::EmittedEvent;
using aegis::exchange::ExchangeNode;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;
using aegis::participant::oms::ExecutionTransport;
using aegis::participant::oms::OrderManager;
using aegis::participant::oms::OrderState;
using aegis::participant::oms::RecordedResponseAdapter;
using aegis::participant::oms::RiskDecision;
using aegis::participant::oms::RiskGate;
using aegis::participant::oms::RiskVerdict;
using aegis::participant::oms::ScriptedResponse;
using aegis::participant::oms::TransportExecutionAdapter;

constexpr std::uint32_t kInstrumentId = 1;
constexpr std::uint64_t kParticipantId = 1;

InstrumentSpec make_spec() {
  InstrumentSpec spec;
  spec.instrument_id = InstrumentId{kInstrumentId};
  spec.price_floor_units = PriceUnits{1000};
  spec.price_ceiling_units = PriceUnits{5000};
  spec.tick_size_units = 25;
  spec.min_quantity_units = QuantityUnits{25};
  spec.max_quantity_units = QuantityUnits{100'000};
  spec.lot_size_units = 25;
  return spec;
}

/// Same test-only transport as the real-exchange integration harness. See
/// that file's header for why this composition is legal only in `tests/`.
class InProcessExchangeTransport final : public ExecutionTransport {
 public:
  InProcessExchangeTransport(ExchangeNode& node, ManualClock& clock)
      : node_(&node), clock_(&clock) {}

  [[nodiscard]] bool send(const Envelope& envelope) override {
    const auto command_sequence =
        node_->sequencer().sequence(clock_->stamp<aegis::common::EventTime>());
    std::vector<EmittedEvent> emitted;
    switch (envelope.message_type) {
      case MessageType::kNewOrder: {
        const auto command = decode_new_order(envelope.payload);
        if (!command.has_value()) {
          return false;
        }
        emitted = node_->apply_new_order(*command, command_sequence);
        break;
      }
      case MessageType::kCancelOrder: {
        const auto command = decode_cancel_order(envelope.payload);
        if (!command.has_value()) {
          return false;
        }
        emitted = node_->apply_cancel_order(*command, command_sequence);
        break;
      }
      case MessageType::kModifyOrder: {
        const auto command = decode_modify_order(envelope.payload);
        if (!command.has_value()) {
          return false;
        }
        emitted = node_->apply_modify_order(*command, command_sequence);
        break;
      }
      default:
        return false;
    }
    pending_.insert(pending_.end(), emitted.begin(), emitted.end());
    return true;
  }

  [[nodiscard]] std::vector<EmittedEvent> drain() {
    std::vector<EmittedEvent> out;
    out.swap(pending_);
    return out;
  }

 private:
  ExchangeNode* node_;
  ManualClock* clock_;
  std::vector<EmittedEvent> pending_;
};

class AlwaysApproveRiskGate final : public RiskGate {
 public:
  [[nodiscard]] RiskDecision decide(const NewOrderCommand& /*command*/) const override {
    return RiskDecision{
        .verdict = RiskVerdict::kApprove, .resized_quantity_units = 0, .reason = ""};
  }
};

/// One `OrderManager` fronted by `TransportExecutionAdapter` over a real
/// `ExchangeNode`. Member order matters: `transport_`/`adapter_` must
/// outlive nothing they don't already own, and `manager` must construct
/// after both `adapter_` and `risk_`.
class RealAdapterHarness {
 public:
  RealAdapterHarness() : transport_(node_, clock_), adapter_(transport_, clock_, /*stream_id=*/1) {
    node_.add_instrument(make_spec());
  }

  [[nodiscard]] OrderManager& manager() { return manager_; }
  [[nodiscard]] std::vector<EmittedEvent> drain() { return transport_.drain(); }

 private:
  ExchangeNode node_;
  ManualClock clock_;
  InProcessExchangeTransport transport_;
  TransportExecutionAdapter adapter_;
  AlwaysApproveRiskGate risk_;
  OrderManager manager_{adapter_, risk_};
};

/// One `OrderManager` fronted by `RecordedResponseAdapter` driven from a
/// committed script.
class RecordedAdapterHarness {
 public:
  explicit RecordedAdapterHarness(std::vector<ScriptedResponse> script)
      : adapter_(std::move(script)) {}

  [[nodiscard]] OrderManager& manager() { return manager_; }
  [[nodiscard]] std::optional<ScriptedResponse> next_response() { return adapter_.next_response(); }

 private:
  RecordedResponseAdapter adapter_;
  AlwaysApproveRiskGate risk_;
  OrderManager manager_{adapter_, risk_};
};

TEST(AdapterContract, NewOrderAcceptedReachesAcknowledgedOnBothAdapters) {
  RealAdapterHarness real;
  const auto real_client_order_id = real.manager().submit_new_order(
      kInstrumentId, kParticipantId, Side::kBuy, OrderType::kLimit, /*price_units=*/1000,
      /*quantity_units=*/50);
  const auto real_emitted = real.drain();
  ASSERT_EQ(real_emitted.size(), 1U);
  ASSERT_EQ(real_emitted.front().message_type, MessageType::kOrderAccepted);
  const auto real_accepted = decode_order_accepted(real_emitted.front().payload);
  ASSERT_TRUE(real_accepted.has_value());
  real.manager().handle_order_accepted(real_accepted.value());
  const auto* real_tracked = real.manager().find_by_client_order_id(real_client_order_id);
  ASSERT_NE(real_tracked, nullptr);

  RecordedAdapterHarness recorded(
      {ScriptedResponse{.message_type = MessageType::kOrderAccepted,
                        .payload = encode(OrderAcceptedEvent{.order_id = 777,
                                                             .instrument_id = kInstrumentId,
                                                             .participant_id = kParticipantId,
                                                             .client_order_id = 1,
                                                             .side = Side::kBuy,
                                                             .order_type = OrderType::kLimit,
                                                             .price_units = 1000,
                                                             .quantity_units = 50})}});
  const auto recorded_client_order_id = recorded.manager().submit_new_order(
      kInstrumentId, kParticipantId, Side::kBuy, OrderType::kLimit, /*price_units=*/1000,
      /*quantity_units=*/50);
  ASSERT_EQ(recorded_client_order_id, real_client_order_id);  // Both managers start fresh at 1.
  const auto recorded_response = recorded.next_response();
  ASSERT_TRUE(recorded_response.has_value());
  const auto recorded_accepted = decode_order_accepted(recorded_response.value().payload);
  ASSERT_TRUE(recorded_accepted.has_value());
  recorded.manager().handle_order_accepted(recorded_accepted.value());
  const auto* recorded_tracked =
      recorded.manager().find_by_client_order_id(recorded_client_order_id);
  ASSERT_NE(recorded_tracked, nullptr);

  EXPECT_EQ(real_tracked->lifecycle.state(), OrderState::kAcknowledged);
  EXPECT_EQ(recorded_tracked->lifecycle.state(), OrderState::kAcknowledged);
  EXPECT_EQ(real_tracked->lifecycle.state(), recorded_tracked->lifecycle.state());
}

TEST(AdapterContract, NewOrderRejectedReachesRejectedOnBothAdapters) {
  RealAdapterHarness real;
  // Below the instrument's price floor (1000): a validation-level rejection.
  const auto real_client_order_id = real.manager().submit_new_order(
      kInstrumentId, kParticipantId, Side::kBuy, OrderType::kLimit, /*price_units=*/25,
      /*quantity_units=*/50);
  const auto real_emitted = real.drain();
  ASSERT_EQ(real_emitted.size(), 1U);
  ASSERT_EQ(real_emitted.front().message_type, MessageType::kOrderRejected);
  const auto real_rejected = decode_order_rejected(real_emitted.front().payload);
  ASSERT_TRUE(real_rejected.has_value());
  real.manager().handle_order_rejected(real_rejected.value());
  const auto* real_tracked = real.manager().find_by_client_order_id(real_client_order_id);
  ASSERT_NE(real_tracked, nullptr);

  RecordedAdapterHarness recorded({ScriptedResponse{
      .message_type = MessageType::kOrderRejected,
      .payload = encode(OrderRejectedEvent{.instrument_id = kInstrumentId,
                                           .participant_id = kParticipantId,
                                           .client_order_id = 1,
                                           .order_id = 0,
                                           .reason = RejectReason::kPriceOutOfBand})}});
  const auto recorded_client_order_id = recorded.manager().submit_new_order(
      kInstrumentId, kParticipantId, Side::kBuy, OrderType::kLimit, /*price_units=*/25,
      /*quantity_units=*/50);
  const auto recorded_response = recorded.next_response();
  ASSERT_TRUE(recorded_response.has_value());
  const auto recorded_rejected = decode_order_rejected(recorded_response.value().payload);
  ASSERT_TRUE(recorded_rejected.has_value());
  recorded.manager().handle_order_rejected(recorded_rejected.value());
  const auto* recorded_tracked =
      recorded.manager().find_by_client_order_id(recorded_client_order_id);
  ASSERT_NE(recorded_tracked, nullptr);

  EXPECT_EQ(real_tracked->lifecycle.state(), OrderState::kRejected);
  EXPECT_EQ(recorded_tracked->lifecycle.state(), OrderState::kRejected);
  EXPECT_EQ(real_tracked->lifecycle.state(), recorded_tracked->lifecycle.state());
}

TEST(AdapterContract, CancelTerminatedReachesCancelledOnBothAdapters) {
  RealAdapterHarness real;
  const auto real_client_order_id = real.manager().submit_new_order(
      kInstrumentId, kParticipantId, Side::kBuy, OrderType::kLimit, /*price_units=*/1000,
      /*quantity_units=*/50);
  {
    const auto emitted = real.drain();
    const auto accepted = decode_order_accepted(emitted.front().payload);
    ASSERT_TRUE(accepted.has_value());
    real.manager().handle_order_accepted(accepted.value());
  }
  ASSERT_TRUE(real.manager().cancel_order(real_client_order_id));
  const auto real_emitted = real.drain();
  ASSERT_EQ(real_emitted.size(), 1U);
  ASSERT_EQ(real_emitted.front().message_type, MessageType::kOrderTerminated);
  const auto real_terminated = decode_order_terminated(real_emitted.front().payload);
  ASSERT_TRUE(real_terminated.has_value());
  real.manager().handle_order_terminated(real_terminated.value());
  const auto* real_tracked = real.manager().find_by_client_order_id(real_client_order_id);
  ASSERT_NE(real_tracked, nullptr);

  RecordedAdapterHarness recorded(
      {ScriptedResponse{.message_type = MessageType::kOrderAccepted,
                        .payload = encode(OrderAcceptedEvent{.order_id = 777,
                                                             .instrument_id = kInstrumentId,
                                                             .participant_id = kParticipantId,
                                                             .client_order_id = 1,
                                                             .side = Side::kBuy,
                                                             .order_type = OrderType::kLimit,
                                                             .price_units = 1000,
                                                             .quantity_units = 50})},
       ScriptedResponse{
           .message_type = MessageType::kOrderTerminated,
           .payload = encode(OrderTerminatedEvent{.order_id = 777,
                                                  .reason = TerminationReason::kCanceled,
                                                  .cancelled_quantity_delta_units = 50})}});
  const auto recorded_client_order_id = recorded.manager().submit_new_order(
      kInstrumentId, kParticipantId, Side::kBuy, OrderType::kLimit, /*price_units=*/1000,
      /*quantity_units=*/50);
  {
    const auto response = recorded.next_response();
    ASSERT_TRUE(response.has_value());
    const auto accepted = decode_order_accepted(response.value().payload);
    ASSERT_TRUE(accepted.has_value());
    recorded.manager().handle_order_accepted(accepted.value());
  }
  ASSERT_TRUE(recorded.manager().cancel_order(recorded_client_order_id));
  const auto recorded_response = recorded.next_response();
  ASSERT_TRUE(recorded_response.has_value());
  const auto recorded_terminated = decode_order_terminated(recorded_response.value().payload);
  ASSERT_TRUE(recorded_terminated.has_value());
  recorded.manager().handle_order_terminated(recorded_terminated.value());
  const auto* recorded_tracked =
      recorded.manager().find_by_client_order_id(recorded_client_order_id);
  ASSERT_NE(recorded_tracked, nullptr);

  EXPECT_EQ(real_tracked->lifecycle.state(), OrderState::kCancelled);
  EXPECT_EQ(recorded_tracked->lifecycle.state(), OrderState::kCancelled);
  EXPECT_EQ(real_tracked->lifecycle.state(), recorded_tracked->lifecycle.state());
}

}  // namespace
