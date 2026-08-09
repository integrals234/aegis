#pragma once

#include <cstdint>

#include "cpp/common/time.hpp"
#include "cpp/events/sequence.hpp"

/// The sole ordering authority (ADR-0012). Assigns `CommandSequence` to every
/// command it receives — including one that will later be rejected — and
/// derives `ExchangeTime` purely from input, never from a clock. Depends only
/// on `cpp-common` and `cpp-events` (`configs/architecture_rules.yaml`), which
/// is what forces the journal to be expressed in envelope terms rather than
/// book terms: the sequencer cannot know what an `OrderId` or a `PriceLevel`
/// is.
namespace aegis::exchange {

class Sequencer {
 public:
  /// `next_command_sequence` starts at 1; the first call to `sequence()`
  /// returns `CommandSequence{1}`.
  Sequencer() = default;

  /// Restores position after a snapshot (ADR-0013). `next_command_sequence`
  /// is the value the *next* call to `sequence()` will return.
  Sequencer(events::CommandSequence next_command_sequence, common::ExchangeTime last_exchange_time)
      : next_command_sequence_(next_command_sequence.value()),
        last_exchange_time_(last_exchange_time) {}

  /// Assigns the next `CommandSequence` and advances `ExchangeTime` to
  /// `max(previous, event_time)` — a regressing input is stamped forward and
  /// counted, never rejected silently.
  events::CommandSequence sequence(common::EventTime event_time);

  [[nodiscard]] events::CommandSequence next_command_sequence() const {
    return events::CommandSequence{next_command_sequence_};
  }
  [[nodiscard]] common::ExchangeTime last_exchange_time() const { return last_exchange_time_; }
  [[nodiscard]] std::uint64_t regressing_event_time_count() const {
    return regressing_event_time_count_;
  }

 private:
  std::uint64_t next_command_sequence_{1};
  common::ExchangeTime last_exchange_time_;
  std::uint64_t regressing_event_time_count_{0};
};

}  // namespace aegis::exchange
