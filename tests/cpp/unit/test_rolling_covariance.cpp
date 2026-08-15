#include <cmath>
#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/statistics/rolling_covariance.hpp"

/// AEGIS-101/102: "matches offline fixture" / "matches offline fixture and
/// edge cases."
namespace {

using aegis::participant::stats::RollingCovariance;

double offline_covariance(const std::deque<std::pair<double, double>>& window) {
  if (window.size() < 2) {
    return 0.0;  // Matches RollingCovariance's own documented edge case.
  }
  double mean_x = 0.0;
  double mean_y = 0.0;
  for (const auto& [x, y] : window) {
    mean_x += x;
    mean_y += y;
  }
  mean_x /= static_cast<double>(window.size());
  mean_y /= static_cast<double>(window.size());
  double sum = 0.0;
  for (const auto& [x, y] : window) {
    sum += (x - mean_x) * (y - mean_y);
  }
  return sum / (static_cast<double>(window.size()) - 1.0);
}

constexpr double kTolerance = 1e-9;

TEST(RollingCovariance, MatchesOfflineCovarianceAfterSlidingPastTheWindow) {
  constexpr std::size_t kWindow = 4;
  RollingCovariance cov(kWindow);
  std::deque<std::pair<double, double>> window;
  const std::vector<std::pair<double, double>> series{{1.0, 2.0}, {2.0, 3.5}, {3.0, 3.0},
                                                      {4.0, 5.0}, {5.0, 4.0}, {6.0, 7.0}};
  for (const auto& [x, y] : series) {
    cov.push(x, y);
    window.emplace_back(x, y);
    if (window.size() > kWindow) {
      window.pop_front();
    }
    EXPECT_NEAR(cov.covariance(), offline_covariance(window), kTolerance);
  }
}

TEST(RollingCovariance, PerfectlyCorrelatedSeriesReportsCorrelationOne) {
  RollingCovariance cov(5);
  for (double v = 1.0; v <= 5.0; v += 1.0) {
    cov.push(v, (2.0 * v) + 3.0);  // y is an exact positive linear function of x.
  }
  EXPECT_NEAR(cov.correlation(), 1.0, kTolerance);
}

TEST(RollingCovariance, InverselyCorrelatedSeriesReportsCorrelationMinusOne) {
  RollingCovariance cov(5);
  for (double v = 1.0; v <= 5.0; v += 1.0) {
    cov.push(v, -v);
  }
  EXPECT_NEAR(cov.correlation(), -1.0, kTolerance);
}

TEST(RollingCovariance, ConstantSeriesReportsZeroCorrelationEdgeCase) {
  RollingCovariance cov(5);
  for (int i = 0; i < 5; ++i) {
    cov.push(3.0, static_cast<double>(i));  // x never moves.
  }
  EXPECT_EQ(cov.correlation(), 0.0);
}

TEST(RollingCovariance, FewerThanTwoObservationsReportsZeroEdgeCase) {
  RollingCovariance cov(5);
  EXPECT_EQ(cov.covariance(), 0.0);
  EXPECT_EQ(cov.correlation(), 0.0);
  cov.push(1.0, 1.0);
  EXPECT_EQ(cov.covariance(), 0.0);
}

}  // namespace
