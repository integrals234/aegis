#include "cpp/exchange/state/event_log.hpp"

namespace aegis::exchange {

events::EventSequence EventLog::append(events::MessageType message_type,
                                       std::vector<std::byte> payload,
                                       common::EventTime event_time) {
  const auto assigned = events::EventSequence{next_event_sequence_};
  ++next_event_sequence_;

  events::Envelope envelope;
  envelope.message_type = message_type;
  envelope.sequence = assigned.value();
  envelope.event_time = event_time;
  envelope.payload = std::move(payload);
  envelopes_.push_back(std::move(envelope));
  return assigned;
}

std::vector<std::string> EventLog::to_canonical_lines() const {
  std::vector<std::string> lines;
  lines.reserve(envelopes_.size());
  for (const auto& envelope : envelopes_) {
    lines.push_back(events::to_hex(events::encode(envelope)));
  }
  return lines;
}

}  // namespace aegis::exchange
