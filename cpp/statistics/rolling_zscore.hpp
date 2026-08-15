#pragma once

#include <cstddef>

#include "cpp/statistics/rolling_moments.hpp"

/// Leakage-free rolling z-score (AEGIS-103; ADR-0022).
namespace aegis::participant::stats {

/// Documented convention: the z-score of an observation is computed against
/// the *prior* window's mean and standard deviation -- the window as it
/// stood immediately before this observation was added, never including it.
/// This is deliberately leakage-free: an observation never influences its
/// own normalisation, which the "current window" alternative (including the
/// point itself before scoring it) would allow.
class RollingZScore {
 public:
  /// Precondition: `window > 0`.
  explicit RollingZScore(std::size_t window) : moments_(window) {}

  /// Scores `value` against the prior window, then adds `value` to the
  /// window for future observations. `0.0` -- a defined edge case -- if the
  /// prior window has fewer than two observations or zero variance (a
  /// constant window), since no deviation is observable to score against.
  [[nodiscard]] double push_and_score(double value);

  [[nodiscard]] std::size_t count() const { return moments_.count(); }

 private:
  RollingMoments moments_;
};

}  // namespace aegis::participant::stats
