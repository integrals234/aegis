#include "cpp/exchange/sequencer/sequencer.hpp"

namespace aegis::exchange {

events::CommandSequence Sequencer::sequence(common::EventTime event_time) {
  const auto assigned = events::CommandSequence{next_command_sequence_};
  ++next_command_sequence_;

  const common::ExchangeTime candidate{event_time.nanos()};
  if (candidate < last_exchange_time_) {
    ++regressing_event_time_count_;
  } else {
    last_exchange_time_ = candidate;
  }
  return assigned;
}

}  // namespace aegis::exchange
