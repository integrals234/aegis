#include "cpp/statistics/drawdown_tracker.hpp"

#include <algorithm>

namespace aegis::participant::stats {

void DrawdownTracker::push(double value) {
  // Expanding-window forward Welford: no eviction, so no reverse half.
  ++count_;
  const auto n = static_cast<double>(count_);
  const double delta = value - mean_;
  mean_ += delta / n;
  const double delta2 = value - mean_;
  m2_ += delta * delta2;

  if (!initialized_) {
    high_water_mark_ = value;
    initialized_ = true;
  } else if (value > high_water_mark_) {
    high_water_mark_ = value;
  }
  last_value_ = value;
  current_drawdown_ = high_water_mark_ - last_value_;
  max_drawdown_ = std::max(current_drawdown_, max_drawdown_);
}

double DrawdownTracker::variance() const {
  if (count_ < 2) {
    return 0.0;
  }
  return m2_ / (static_cast<double>(count_) - 1.0);
}

}  // namespace aegis::participant::stats
