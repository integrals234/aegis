#pragma once

#include <cstddef>
#include <deque>
#include <utility>

/// Generic online covariance/correlation (AEGIS-101, AEGIS-102; ADR-0022).
///
/// Same discipline as `RollingMoments`: `cpp-statistics.may_depend_on =
/// [cpp-common]` only, plain numeric observations, sample statistics
/// (`ddof = 1`), reverse-Welford eviction for numerical stability under a
/// sliding window.
namespace aegis::participant::stats {

/// Tracks the co-moment of a paired series `(x, y)` alongside each series'
/// own variance, using the bivariate generalisation of the Welford
/// recursion (both forward add and reverse remove) -- the standard
/// numerically stable streaming covariance update, not an approximation.
class RollingCovariance {
 public:
  /// Precondition: `window > 0`.
  explicit RollingCovariance(std::size_t window) : window_(window) {}

  /// Adds `(x, y)`, evicting the oldest pair first if the window is full.
  void push(double x, double y);

  [[nodiscard]] std::size_t count() const { return buffer_.size(); }

  [[nodiscard]] double mean_x() const { return mean_x_; }
  [[nodiscard]] double mean_y() const { return mean_y_; }

  /// Sample variance/covariance (`ddof = 1`). `0.0` for fewer than two
  /// observations.
  [[nodiscard]] double variance_x() const;
  [[nodiscard]] double variance_y() const;
  [[nodiscard]] double covariance() const;

  /// `covariance() / sqrt(variance_x() * variance_y())`, in `[-1, 1]`.
  /// `0.0` — a defined edge case, not `NaN` — whenever either series is
  /// constant over the window (zero variance), since no correlation is
  /// observable from a series that never moves.
  [[nodiscard]] double correlation() const;

 private:
  std::size_t window_;
  std::deque<std::pair<double, double>> buffer_;
  double mean_x_{0.0};
  double mean_y_{0.0};
  double m2_x_{0.0};  ///< Sum of squared deviations of x (Welford).
  double m2_y_{0.0};  ///< Sum of squared deviations of y (Welford).
  double c_xy_{0.0};  ///< Sum of co-deviations of (x, y) (Welford).
};

}  // namespace aegis::participant::stats
