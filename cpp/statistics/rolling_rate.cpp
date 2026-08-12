#include "cpp/statistics/rolling_rate.hpp"

namespace aegis::participant::stats {

void RollingRate::record_event(common::Nanos timestamp_nanos) {
  timestamps_.push_back(timestamp_nanos);
  while (!timestamps_.empty() && timestamp_nanos - timestamps_.front() > window_.nanos()) {
    timestamps_.pop_front();
  }
}

double RollingRate::rate_per_second() const {
  if (timestamps_.empty()) {
    return 0.0;
  }
  const double window_seconds = window_.seconds();
  return window_seconds > 0.0 ? static_cast<double>(timestamps_.size()) / window_seconds : 0.0;
}

}  // namespace aegis::participant::stats
