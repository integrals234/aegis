#include "cpp/participant/oms/transport_execution_adapter.hpp"

#include <utility>

namespace aegis::participant::oms {

events::Envelope TransportExecutionAdapter::frame(events::MessageType type,
                                                  std::vector<std::byte> payload) {
  events::Envelope envelope;
  envelope.message_type = type;
  envelope.sequence = next_sequence_++;
  envelope.stream_id = stream_id_;
  envelope.event_time = clock_->stamp<common::EventTime>();
  envelope.payload = std::move(payload);
  return envelope;
}

bool TransportExecutionAdapter::submit(const events::exchange::NewOrderCommand& command) {
  return transport_->send(frame(events::MessageType::kNewOrder, encode(command)));
}

bool TransportExecutionAdapter::cancel(const events::exchange::CancelOrderCommand& command) {
  return transport_->send(frame(events::MessageType::kCancelOrder, encode(command)));
}

bool TransportExecutionAdapter::modify(const events::exchange::ModifyOrderCommand& command) {
  return transport_->send(frame(events::MessageType::kModifyOrder, encode(command)));
}

}  // namespace aegis::participant::oms
