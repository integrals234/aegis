#include <gtest/gtest.h>

#include "cpp/statistics/rolling_zscore.hpp"

/// AEGIS-103: "timestamped fixture passes," and the leakage-free convention
/// ADR-0022 documents.
namespace {

using aegis::participant::stats::RollingZScore;

TEST(RollingZScore, FirstObservationScoresZeroEdgeCase) {
  RollingZScore z(5);
  EXPECT_EQ(z.push_and_score(42.0), 0.0);  // No prior window to score against.
}

TEST(RollingZScore, IsLeakageFreeAgainstThePriorWindowOnly) {
  RollingZScore z(3);
  static_cast<void>(z.push_and_score(1.0));
  static_cast<void>(z.push_and_score(2.0));
  static_cast<void>(z.push_and_score(3.0));
  // Prior window is now {1, 2, 3}: mean 2, sample stddev 1.
  const double score = z.push_and_score(10.0);
  EXPECT_NEAR(score, (10.0 - 2.0) / 1.0, 1e-9);
}

TEST(RollingZScore, ConstantPriorWindowScoresZeroEdgeCase) {
  RollingZScore z(3);
  static_cast<void>(z.push_and_score(5.0));
  static_cast<void>(z.push_and_score(5.0));
  const double score = z.push_and_score(5.0);  // Prior window {5,5}: stddev 0.
  EXPECT_EQ(score, 0.0);
}

TEST(RollingZScore, TheScoredValueItselfNeverAffectsItsOwnScore) {
  // Prior window {1, 2, 3}: mean 2, sample stddev 1. A leaky implementation
  // that included the new value in its own normalisation would score this
  // outlier far smaller than (1000-2)/1 -- the value would inflate the very
  // stddev used to judge it.
  RollingZScore z(4);
  static_cast<void>(z.push_and_score(1.0));
  static_cast<void>(z.push_and_score(2.0));
  static_cast<void>(z.push_and_score(3.0));
  const double score = z.push_and_score(1000.0);
  EXPECT_NEAR(score, (1000.0 - 2.0) / 1.0, 1e-9);
}

}  // namespace
