#pragma once

#include <vector>

#include "cpp/common/clock.hpp"
#include "cpp/events/envelope.hpp"
#include "cpp/events/exchange_messages.hpp"
#include "cpp/exchange/app/exchange_node.hpp"
#include "cpp/participant/oms/execution_transport.hpp"

/// Real M1 exchange integration, composed entirely inside `tests/` --
/// outside `covered_roots` (`configs/architecture_rules.yaml`), so this file
/// may legally see both the exchange and the participant at once without
/// creating a production participant -> exchange dependency (ADR-0023,
/// MASTER_SPEC immutable principle 1). This is the *only* class anywhere
/// that does this; it is exercised by GoogleTest, never linked into
/// `aegis_participant_run` or any other production target.
///
/// The chain under test, wherever this is used: OMS intent ->
/// `TransportExecutionAdapter` -> this transport -> a real `ExchangeNode` ->
/// real FIFO matching -> the resulting `EmittedEvent`s decoded and fed back
/// into `OrderManager`/`Portfolio`, exactly as a production caller would
/// decode them from the wire. Real M1 matching/FIFO/residual semantics are
/// exercised unmodified.
namespace aegis::testing {

class InProcessExchangeTransport final : public aegis::participant::oms::ExecutionTransport {
 public:
  InProcessExchangeTransport(aegis::exchange::ExchangeNode& node, aegis::common::ManualClock& clock)
      : node_(&node), clock_(&clock) {}

  [[nodiscard]] bool send(const aegis::events::Envelope& envelope) override {
    const auto command_sequence =
        node_->sequencer().sequence(clock_->stamp<aegis::common::EventTime>());
    std::vector<aegis::exchange::EmittedEvent> emitted;
    switch (envelope.message_type) {
      case aegis::events::MessageType::kNewOrder: {
        const auto command = aegis::events::exchange::decode_new_order(envelope.payload);
        if (!command.has_value()) {
          return false;
        }
        emitted = node_->apply_new_order(*command, command_sequence);
        break;
      }
      case aegis::events::MessageType::kCancelOrder: {
        const auto command = aegis::events::exchange::decode_cancel_order(envelope.payload);
        if (!command.has_value()) {
          return false;
        }
        emitted = node_->apply_cancel_order(*command, command_sequence);
        break;
      }
      case aegis::events::MessageType::kModifyOrder: {
        const auto command = aegis::events::exchange::decode_modify_order(envelope.payload);
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

  /// Drains every `EmittedEvent` accumulated since the last drain, in order.
  [[nodiscard]] std::vector<aegis::exchange::EmittedEvent> drain() {
    std::vector<aegis::exchange::EmittedEvent> out;
    out.swap(pending_);
    return out;
  }

 private:
  aegis::exchange::ExchangeNode* node_;
  aegis::common::ManualClock* clock_;
  std::vector<aegis::exchange::EmittedEvent> pending_;
};

}  // namespace aegis::testing
