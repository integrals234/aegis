#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <numeric>

#include <gtest/gtest.h>

#include "cpp/statistics/rolling_moments.hpp"

namespace {

using aegis::participant::stats::RollingMoments;

/// Trusted two-pass offline calculation over whatever is currently in
/// `window` -- the fixture AEGIS-098/099/100's frozen acceptance
/// ("matches trusted offline calculations") asks the online estimator to
/// agree with.
double offline_mean(const std::deque<double>& window) {
  return std::accumulate(window.begin(), window.end(), 0.0) / static_cast<double>(window.size());
}

double offline_variance(const std::deque<double>& window) {
  if (window.size() < 2) {
    return 0.0;
  }
  const double mean = offline_mean(window);
  double sum_sq = 0.0;
  for (const double value : window) {
    sum_sq += (value - mean) * (value - mean);
  }
  return sum_sq / (static_cast<double>(window.size()) - 1.0);
}

constexpr double kTolerance = 1e-9;

TEST(RollingMoments, MatchesOfflineCalculationWhileFillingTheWindow) {
  RollingMoments moments(4);
  std::deque<double> window;
  for (const double value : {2.0, 4.0, 4.0, 4.0}) {
    moments.push(value);
    window.push_back(value);
    EXPECT_NEAR(moments.mean(), offline_mean(window), kTolerance);
    EXPECT_NEAR(moments.variance(), offline_variance(window), kTolerance);
  }
}

TEST(RollingMoments, MatchesOfflineCalculationAfterSlidingPastTheWindow) {
  constexpr std::size_t kWindow = 5;
  RollingMoments moments(kWindow);
  std::deque<double> window;
  for (const double value : {1.0, 3.0, 5.0, 7.0, 9.0, 11.0, 2.0, 8.0, 6.0, 4.0}) {
    moments.push(value);
    window.push_back(value);
    if (window.size() > kWindow) {
      window.pop_front();
    }
    EXPECT_NEAR(moments.mean(), offline_mean(window), kTolerance);
    EXPECT_NEAR(moments.stddev(), std::sqrt(offline_variance(window)), kTolerance);
    EXPECT_EQ(moments.count(), window.size());
  }
}

TEST(RollingMoments, AdversarialLargeOffsetStaysNumericallyStable) {
  // A large common offset with a small spread is exactly the case naive
  // sum/sum-of-squares recomputation loses precision on; Welford add/remove
  // should not.
  constexpr std::size_t kWindow = 6;
  RollingMoments moments(kWindow);
  std::deque<double> window;
  const std::array<double, 12> values{1'000'000.0, 1'000'001.0, 1'000'002.0, 1'000'001.5,
                                      1'000'000.5, 1'000'001.0, 1'000'002.5, 1'000'000.2,
                                      1'000'001.8, 1'000'000.9, 1'000'001.3, 1'000'000.7};
  for (const double value : values) {
    moments.push(value);
    window.push_back(value);
    if (window.size() > kWindow) {
      window.pop_front();
    }
    EXPECT_NEAR(moments.variance(), offline_variance(window), 1e-6);
  }
}

TEST(RollingMoments, EmptyWindowReportsZeroEdgeCase) {
  RollingMoments moments(3);
  EXPECT_EQ(moments.count(), 0U);
  EXPECT_EQ(moments.mean(), 0.0);
  EXPECT_EQ(moments.variance(), 0.0);
  EXPECT_EQ(moments.stddev(), 0.0);
}

TEST(RollingMoments, SingleObservationHasZeroVarianceEdgeCase) {
  RollingMoments moments(3);
  moments.push(42.0);
  EXPECT_EQ(moments.count(), 1U);
  EXPECT_EQ(moments.mean(), 42.0);
  EXPECT_EQ(moments.variance(), 0.0);
}

}  // namespace
