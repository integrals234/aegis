#include "cpp/statistics/rolling_moments.hpp"

#include <cmath>

namespace aegis::participant::stats {

void RollingMoments::push(double value) {
  if (buffer_.size() == window_) {
    const double evicted = buffer_.front();
    buffer_.pop_front();
    const auto n_before = static_cast<double>(buffer_.size() + 1);
    const auto n_after = static_cast<double>(buffer_.size());
    if (n_after == 0.0) {
      mean_ = 0.0;
      m2_ = 0.0;
    } else {
      // Reverse Welford: the algebraic inverse of the forward update below,
      // derived directly from it -- not an approximation (ADR-0022).
      const double mean_prev = (n_before * mean_ - evicted) / n_after;
      m2_ = m2_ - (evicted - mean_prev) * (evicted - mean_);
      mean_ = mean_prev;
    }
  }

  buffer_.push_back(value);
  const auto n = static_cast<double>(buffer_.size());
  const double delta = value - mean_;
  mean_ += delta / n;
  const double delta2 = value - mean_;
  m2_ += delta * delta2;
}

double RollingMoments::variance() const {
  if (buffer_.size() < 2) {
    return 0.0;
  }
  return m2_ / (static_cast<double>(buffer_.size()) - 1.0);
}

double RollingMoments::stddev() const { return std::sqrt(variance()); }

}  // namespace aegis::participant::stats
