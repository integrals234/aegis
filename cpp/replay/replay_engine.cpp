#include "cpp/replay/replay_engine.hpp"

#include <stdexcept>
#include <utility>

namespace aegis::replay {

ReplayEngine::ReplayEngine(std::vector<ReplayEvent> events, VirtualClock& clock)
    : events_(std::move(events)), clock_(&clock) {}

std::optional<ReplayEvent> ReplayEngine::next() {
  if (!has_next()) {
    return std::nullopt;
  }
  const auto& event = events_[position_];
  clock_->advance_to(event.event_time);
  ++position_;
  return event;
}

std::vector<ReplayEvent> ReplayEngine::next_group() {
  std::vector<ReplayEvent> group;
  if (!has_next()) {
    return group;
  }
  const auto group_time = events_[position_].event_time;
  while (has_next() && events_[position_].event_time == group_time) {
    auto emitted = next();
    group.push_back(
        *emitted);  // NOLINT(bugprone-unchecked-optional-access) - has_next() just checked
  }
  return group;
}

std::optional<RecordIndex> ReplayEngine::cursor() const {
  if (position_ == 0) {
    return std::nullopt;
  }
  return events_[position_ - 1].record_index;
}

void ReplayEngine::resume_from(RecordIndex index) {
  for (std::size_t i = 0; i < events_.size(); ++i) {
    if (events_[i].record_index == index) {
      position_ = i + 1;
      return;
    }
  }
  throw std::invalid_argument("ReplayEngine::resume_from: record_index not found in loaded stream");
}

}  // namespace aegis::replay
