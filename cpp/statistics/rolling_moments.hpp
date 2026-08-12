#pragma once

#include <cstddef>
#include <deque>

/// Generic online rolling statistics (AEGIS-098, AEGIS-099, AEGIS-100;
/// ADR-0022).
///
/// `cpp-statistics.may_depend_on = [cpp-common]` only
/// (`configs/architecture_rules.yaml`) -- this library knows nothing of
/// books, orders, feeds or participants, so M4-M7 can reuse it without
/// inheriting participant-domain types. Its public surface is plain numeric
/// observations; a caller extracts whatever number it cares about (a trade
/// price, a fill quantity) before calling `push`.
namespace aegis::participant::stats {

/// Fixed-window mean, (sample) variance and standard deviation, updated
/// incrementally on both push and eviction rather than recomputed from the
/// window's contents (ADR-0022).
///
/// Uses the reverse-Welford update on eviction: the algebraic inverse of the
/// forward Welford update, not an approximation, which is what "numerically
/// stable add/remove logic" (AEGIS-099's frozen acceptance) means concretely
/// for a *sliding* window rather than a merely expanding one.
class RollingMoments {
 public:
  /// Precondition: `window > 0`.
  explicit RollingMoments(std::size_t window) : window_(window) {}

  /// Adds `value`, evicting the oldest observation first if the window is
  /// already full.
  void push(double value);

  [[nodiscard]] std::size_t count() const { return buffer_.size(); }
  [[nodiscard]] std::size_t window() const { return window_; }

  [[nodiscard]] double mean() const { return mean_; }

  /// Sample variance (`ddof = 1`). `0.0` for fewer than two observations —
  /// an explicit, tested edge case, not an artifact of dividing by zero.
  [[nodiscard]] double variance() const;

  /// `std::sqrt(variance())`. `0.0` under the same edge case as `variance()`.
  [[nodiscard]] double stddev() const;

 private:
  std::size_t window_;
  std::deque<double> buffer_;
  double mean_{0.0};
  double m2_{0.0};  ///< Sum of squared deviations from `mean_` (Welford).
};

}  // namespace aegis::participant::stats
