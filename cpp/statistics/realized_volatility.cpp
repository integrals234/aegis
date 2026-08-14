#include "cpp/statistics/realized_volatility.hpp"

#include <cmath>

namespace aegis::participant::stats {

void RollingRealizedVolatility::push(double return_value) {
  if (buffer_.size() == window_) {
    const double evicted = buffer_.front();
    buffer_.pop_front();
    sum_squares_ -= evicted * evicted;
  }
  buffer_.push_back(return_value);
  sum_squares_ += return_value * return_value;
}

double RollingRealizedVolatility::realized_volatility(double periods_per_year) const {
  if (buffer_.empty()) {
    return 0.0;
  }
  const double mean_square = sum_squares_ / static_cast<double>(buffer_.size());
  return std::sqrt(mean_square) * std::sqrt(periods_per_year);
}

void RollingBeta::push(double asset_return, double benchmark_return) {
  covariance_.push(asset_return, benchmark_return);
}

double RollingBeta::beta() const {
  const double benchmark_variance = covariance_.variance_y();
  if (benchmark_variance <= 0.0) {
    return 0.0;
  }
  return covariance_.covariance() / benchmark_variance;
}

}  // namespace aegis::participant::stats
