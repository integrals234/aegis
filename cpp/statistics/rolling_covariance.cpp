#include "cpp/statistics/rolling_covariance.hpp"

#include <cmath>

namespace aegis::participant::stats {

void RollingCovariance::push(double x, double y) {
  if (buffer_.size() == window_) {
    const auto [evicted_x, evicted_y] = buffer_.front();
    buffer_.pop_front();
    const auto n_before = static_cast<double>(buffer_.size() + 1);
    const auto n_after = static_cast<double>(buffer_.size());
    if (n_after == 0.0) {
      mean_x_ = 0.0;
      mean_y_ = 0.0;
      m2_x_ = 0.0;
      m2_y_ = 0.0;
      c_xy_ = 0.0;
    } else {
      // Bivariate reverse Welford: the algebraic inverse of the forward
      // update below, in the same style as RollingMoments (ADR-0022).
      const double mean_x_prev = (n_before * mean_x_ - evicted_x) / n_after;
      const double mean_y_prev = (n_before * mean_y_ - evicted_y) / n_after;
      m2_x_ -= (evicted_x - mean_x_prev) * (evicted_x - mean_x_);
      m2_y_ -= (evicted_y - mean_y_prev) * (evicted_y - mean_y_);
      c_xy_ -= (evicted_x - mean_x_prev) * (evicted_y - mean_y_);
      mean_x_ = mean_x_prev;
      mean_y_ = mean_y_prev;
    }
  }

  buffer_.push_back({x, y});
  const auto n = static_cast<double>(buffer_.size());
  const double dx = x - mean_x_;
  const double dy = y - mean_y_;
  mean_x_ += dx / n;
  mean_y_ += dy / n;
  const double dx2 = x - mean_x_;
  const double dy2 = y - mean_y_;
  m2_x_ += dx * dx2;
  m2_y_ += dy * dy2;
  c_xy_ += dx * dy2;
}

double RollingCovariance::variance_x() const {
  if (buffer_.size() < 2) {
    return 0.0;
  }
  return m2_x_ / (static_cast<double>(buffer_.size()) - 1.0);
}

double RollingCovariance::variance_y() const {
  if (buffer_.size() < 2) {
    return 0.0;
  }
  return m2_y_ / (static_cast<double>(buffer_.size()) - 1.0);
}

double RollingCovariance::covariance() const {
  if (buffer_.size() < 2) {
    return 0.0;
  }
  return c_xy_ / (static_cast<double>(buffer_.size()) - 1.0);
}

double RollingCovariance::correlation() const {
  const double var_x = variance_x();
  const double var_y = variance_y();
  const double denominator = std::sqrt(var_x * var_y);
  if (denominator <= 0.0) {
    return 0.0;
  }
  return covariance() / denominator;
}

}  // namespace aegis::participant::stats
