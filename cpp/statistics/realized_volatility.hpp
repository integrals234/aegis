#pragma once

#include <cstddef>
#include <deque>

#include "cpp/statistics/rolling_covariance.hpp"

/// Realized volatility and rolling beta (AEGIS-105; ADR-0022).
namespace aegis::participant::stats {

/// Documented convention: realized volatility is the root-mean-square of
/// the returns in the window -- `sqrt(mean(r^2))`, uncentered (no mean
/// subtraction) -- the standard definition for high-frequency returns,
/// which are assumed close to zero-mean rather than re-estimated per
/// window. This differs deliberately from `RollingMoments::stddev()`
/// (which centers on the sample mean); a caller that wants the centered
/// form uses `RollingMoments` directly on the same return series.
class RollingRealizedVolatility {
 public:
  /// Precondition: `window > 0`.
  explicit RollingRealizedVolatility(std::size_t window) : window_(window) {}

  void push(double return_value);

  [[nodiscard]] std::size_t count() const { return buffer_.size(); }

  /// `sqrt(mean(r^2)) * sqrt(periods_per_year)`. `0.0` with no observations.
  /// `periods_per_year` defaults to `1.0` (no annualisation); a caller
  /// scores daily returns annualized by passing `252.0`, for example --
  /// this class does not guess a sampling frequency.
  [[nodiscard]] double realized_volatility(double periods_per_year = 1.0) const;

 private:
  std::size_t window_;
  std::deque<double> buffer_;
  double sum_squares_{0.0};
};

/// `beta = covariance(asset, benchmark) / variance(benchmark)`, built
/// directly on `RollingCovariance` rather than duplicating its
/// bivariate Welford recursion.
class RollingBeta {
 public:
  /// Precondition: `window > 0`.
  explicit RollingBeta(std::size_t window) : covariance_(window) {}

  void push(double asset_return, double benchmark_return);

  [[nodiscard]] std::size_t count() const { return covariance_.count(); }

  /// `0.0` -- a defined edge case, not a division fault -- when the
  /// benchmark has zero variance over the window (fewer than two
  /// observations, or a constant benchmark).
  [[nodiscard]] double beta() const;

 private:
  RollingCovariance covariance_;
};

}  // namespace aegis::participant::stats
