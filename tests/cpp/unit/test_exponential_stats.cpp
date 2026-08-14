#include <gtest/gtest.h>

#include "cpp/statistics/exponential_stats.hpp"

/// AEGIS-104: "numeric fixtures pass," including the documented
/// initialization and decay conventions (ADR-0022).
namespace {

using aegis::participant::stats::ExponentialStats;

TEST(ExponentialStats, FirstObservationInitializesMeanAndZeroesVariance) {
  ExponentialStats stats(0.5);
  EXPECT_FALSE(stats.has_value());
  stats.push(10.0);
  EXPECT_TRUE(stats.has_value());
  EXPECT_EQ(stats.mean(), 10.0);
  EXPECT_EQ(stats.variance(), 0.0);
}

TEST(ExponentialStats, MeanMatchesHandComputedRecursionForTwoSteps) {
  ExponentialStats stats(0.5);
  stats.push(10.0);  // mean = 10.
  stats.push(20.0);  // mean += 0.5*(20-10) = 15.
  EXPECT_DOUBLE_EQ(stats.mean(), 15.0);
  stats.push(20.0);  // mean += 0.5*(20-15) = 17.5.
  EXPECT_DOUBLE_EQ(stats.mean(), 17.5);
}

TEST(ExponentialStats, VarianceMatchesHandComputedRecursionForThreeSteps) {
  // diff = x - mean; incr = alpha*diff; mean += incr;
  // variance = (1-alpha)*(variance + diff*incr).
  ExponentialStats stats(0.5);
  stats.push(10.0);  // mean=10, var=0.
  stats.push(20.0);  // diff=10, incr=5, mean=15, var=0.5*(0+10*5)=25.
  EXPECT_DOUBLE_EQ(stats.mean(), 15.0);
  EXPECT_DOUBLE_EQ(stats.variance(), 25.0);
  stats.push(10.0);  // diff=-5, incr=-2.5, mean=12.5, var=0.5*(25+(-5)*(-2.5))=18.75.
  EXPECT_DOUBLE_EQ(stats.mean(), 12.5);
  EXPECT_DOUBLE_EQ(stats.variance(), 18.75);
}

TEST(ExponentialStats, StddevIsSqrtOfVariance) {
  ExponentialStats stats(0.5);
  stats.push(10.0);
  stats.push(20.0);
  EXPECT_DOUBLE_EQ(stats.stddev(), 5.0);  // sqrt(25).
}

TEST(ExponentialStats, ConstantSeriesHasZeroVariance) {
  ExponentialStats stats(0.3);
  for (int i = 0; i < 5; ++i) {
    stats.push(7.0);
  }
  EXPECT_EQ(stats.variance(), 0.0);
  EXPECT_EQ(stats.mean(), 7.0);
}

}  // namespace
