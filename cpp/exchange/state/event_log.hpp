#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/events/envelope.hpp"
#include "cpp/events/sequence.hpp"

/// The canonical event stream (ADR-0012, ADR-0009). `EventLog` owns
/// `next_event_sequence` — one counter for the whole exchange run, so an
/// accept that also produces trades and terminations advances it once per
/// event, not once per command — and frames every emitted event in an
/// `Envelope` whose `sequence` field carries that `EventSequence`.
namespace aegis::exchange {

class EventLog {
 public:
  EventLog() = default;

  /// Restores position after a snapshot (ADR-0013). `next_event_sequence` is
  /// the value the *next* call to `append` will assign.
  explicit EventLog(events::EventSequence next_event_sequence)
      : next_event_sequence_(next_event_sequence.value()) {}

  /// Frames `payload` (an encoded exchange event struct from
  /// `cpp/events/exchange_messages.hpp`) as a canonical `Envelope`, assigns it
  /// the next `EventSequence`, and appends it. Returns the assigned sequence.
  events::EventSequence append(events::MessageType message_type, std::vector<std::byte> payload,
                               common::EventTime event_time);

  [[nodiscard]] const std::vector<events::Envelope>& envelopes() const { return envelopes_; }
  [[nodiscard]] events::EventSequence next_event_sequence() const {
    return events::EventSequence{next_event_sequence_};
  }

  /// One canonical hex line per envelope, in `EventSequence` order — the form
  /// fixtures and the determinism harness compare (AEGIS-005).
  [[nodiscard]] std::vector<std::string> to_canonical_lines() const;

 private:
  std::uint64_t next_event_sequence_{1};
  std::vector<events::Envelope> envelopes_;
};

}  // namespace aegis::exchange
