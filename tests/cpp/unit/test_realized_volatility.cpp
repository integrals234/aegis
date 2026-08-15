#include <cmath>
#include <deque>

#include <gtest/gtest.h>

#include "cpp/statistics/realized_volatility.hpp"

/// AEGIS-105: "offline equivalence tests pass."
namespace {

using aegis::participant::stats::RollingBeta;
using aegis::participant::stats::RollingRealizedVolatility;

double offline_realized_volatility(const std::deque<double>& returns) {
  double sum_squares = 0.0;
  for (const double r : returns) {
    sum_squares += r * r;
  }
  return std::sqrt(sum_squares / static_cast<double>(returns.size()));
}

TEST(RollingRealizedVolatility, MatchesOfflineRootMeanSquareAfterSlidingPastTheWindow) {
  constexpr std::size_t kWindow = 4;
  RollingRealizedVolatility vol(kWindow);
  std::deque<double> window;
  for (const double r : {0.01, -0.02, 0.015, -0.005, 0.03, -0.01, 0.02}) {
    vol.push(r);
    window.push_back(r);
    if (window.size() > kWindow) {
      window.pop_front();
    }
    EXPECT_NEAR(vol.realized_volatility(), offline_realized_volatility(window), 1e-12);
  }
}

TEST(RollingRealizedVolatility, AnnualizationScalesBySqrtOfPeriodsPerYear) {
  RollingRealizedVolatility vol(4);
  vol.push(0.01);
  vol.push(-0.01);
  const double unscaled = vol.realized_volatility();
  const double annualized = vol.realized_volatility(252.0);
  EXPECT_NEAR(annualized, unscaled * std::sqrt(252.0), 1e-12);
}

TEST(RollingRealizedVolatility, EmptyWindowReportsZeroEdgeCase) {
  RollingRealizedVolatility vol(4);
  EXPECT_EQ(vol.realized_volatility(), 0.0);
}

TEST(RollingBeta, MatchesExpectedSlopeForAnExactLinearRelationship) {
  RollingBeta beta(5);
  // asset = 2 * benchmark exactly -> beta should be 2.
  for (int step = 1; step <= 5; ++step) {
    const auto b = static_cast<double>(step);
    beta.push(2.0 * b, b);
  }
  EXPECT_NEAR(beta.beta(), 2.0, 1e-9);
}

TEST(RollingBeta, ZeroBenchmarkVarianceReportsZeroEdgeCase) {
  RollingBeta beta(5);
  for (int i = 0; i < 5; ++i) {
    beta.push(static_cast<double>(i), 3.0);  // Benchmark never moves.
  }
  EXPECT_EQ(beta.beta(), 0.0);
}

}  // namespace
