#pragma once

#include <cstdint>

/// Online drawdown and P&L moments (AEGIS-106; ADR-0022).
namespace aegis::participant::stats {

/// Tracks a cumulative value series (equity or cumulative P&L) with an
/// *expanding*, not sliding, window -- unlike every other estimator in this
/// library, a high-water mark and its mean/variance are all-time quantities
/// by definition, so there is no window size to configure and no eviction.
/// Mean/variance use the same forward-only Welford recursion `RollingMoments`
/// uses for its sliding window, just without the reverse half (ADR-0022).
/// Higher moments beyond variance are not tracked: nothing in M3 needs them,
/// and adding them unused would be exactly the "implement it because it
/// might be needed" this project avoids.
class DrawdownTracker {
 public:
  DrawdownTracker() = default;

  /// Records one observation of the value series.
  void push(double value);

  [[nodiscard]] std::uint64_t count() const { return count_; }
  [[nodiscard]] double mean() const { return mean_; }
  /// Sample variance (`ddof = 1`) of the pushed values. `0.0` for fewer
  /// than two observations.
  [[nodiscard]] double variance() const;

  /// The highest value observed so far. `0.0` before any push -- a
  /// documented starting point, not a claim that the series began at zero.
  [[nodiscard]] double high_water_mark() const { return high_water_mark_; }

  /// `high_water_mark() - <most recent value>`, always `>= 0`.
  [[nodiscard]] double current_drawdown() const { return current_drawdown_; }

  /// The largest `current_drawdown()` has ever been.
  [[nodiscard]] double max_drawdown() const { return max_drawdown_; }

 private:
  std::uint64_t count_{0};
  double mean_{0.0};
  double m2_{0.0};

  bool initialized_{false};
  double high_water_mark_{0.0};
  double last_value_{0.0};
  double current_drawdown_{0.0};
  double max_drawdown_{0.0};
};

}  // namespace aegis::participant::stats
