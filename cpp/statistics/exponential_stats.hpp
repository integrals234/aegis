#pragma once

/// Exponentially weighted mean and variance (AEGIS-104; ADR-0022).
namespace aegis::participant::stats {

/// Documented decay convention: `alpha` is the weight given to the newest
/// observation directly (the "smoothing factor" convention), not a span or
/// half-life -- a caller converts if it wants one of those (`alpha = 2 /
/// (span + 1)`, `alpha = 1 - exp(ln(0.5) / halflife)`), so this class fixes
/// exactly one parameterisation rather than silently supporting several.
///
/// Variance uses the recursive form from Finch, "Incremental Calculation of
/// Weighted Mean and Variance" (2009): `diff = x - mean; incr = alpha *
/// diff; mean += incr; variance = (1 - alpha) * (variance + diff * incr)`.
/// This is exact for the exponentially-weighted second moment, not an
/// approximation -- the same standing ADR-0022 gives every other estimator
/// in this library.
class ExponentialStats {
 public:
  /// Precondition: `0.0 < alpha <= 1.0`.
  explicit ExponentialStats(double alpha) : alpha_(alpha) {}

  /// Documented initialization: the first observation sets `mean = value`
  /// and `variance = 0.0` outright (there is no prior deviation to weight)
  /// rather than seeding from an arbitrary prior.
  void push(double value);

  [[nodiscard]] bool has_value() const { return initialized_; }
  [[nodiscard]] double mean() const { return mean_; }
  [[nodiscard]] double variance() const { return variance_; }
  [[nodiscard]] double stddev() const;

 private:
  double alpha_;
  bool initialized_{false};
  double mean_{0.0};
  double variance_{0.0};
};

}  // namespace aegis::participant::stats
