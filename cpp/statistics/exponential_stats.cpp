#include "cpp/statistics/exponential_stats.hpp"

#include <cmath>

namespace aegis::participant::stats {

void ExponentialStats::push(double value) {
  if (!initialized_) {
    mean_ = value;
    variance_ = 0.0;
    initialized_ = true;
    return;
  }
  const double diff = value - mean_;
  const double incr = alpha_ * diff;
  mean_ += incr;
  variance_ = (1.0 - alpha_) * (variance_ + (diff * incr));
}

double ExponentialStats::stddev() const { return std::sqrt(variance_); }

}  // namespace aegis::participant::stats
