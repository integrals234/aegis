#include <gtest/gtest.h>

#include "cpp/participant/oms/queue_position_estimator.hpp"

/// AEGIS-115: "model limitations are explicit and synthetic validation
/// exists." This is entirely synthetic -- no order-level truth is consulted,
/// only the documented approximation.
namespace {

using aegis::participant::oms::QueuePositionEstimator;

TEST(QueuePositionEstimator, EffectiveVolumeAheadAppliesTheAssumedCancellationRate) {
  const auto estimate = QueuePositionEstimator::estimate(
      /*observed_volume_ahead_units=*/1000, /*assumed_cancellation_rate=*/0.3,
      /*traded_volume_since_units=*/0);
  EXPECT_EQ(estimate.effective_volume_ahead_units, 700);
}

TEST(QueuePositionEstimator, FillProbabilityGrowsWithTradedVolumeSince) {
  const auto half = QueuePositionEstimator::estimate(1000, 0.0, 500);
  EXPECT_DOUBLE_EQ(half.fill_probability, 0.5);

  const auto none = QueuePositionEstimator::estimate(1000, 0.0, 0);
  EXPECT_DOUBLE_EQ(none.fill_probability, 0.0);
}

TEST(QueuePositionEstimator, FillProbabilityIsClampedAtOne) {
  const auto estimate = QueuePositionEstimator::estimate(100, 0.0, 10'000);
  EXPECT_DOUBLE_EQ(estimate.fill_probability, 1.0);
}

TEST(QueuePositionEstimator, NoEffectiveVolumeAheadReportsCertainFillEdgeCase) {
  // 100% assumed cancellation: nothing effectively ahead, already at the front.
  const auto estimate = QueuePositionEstimator::estimate(1000, 1.0, 0);
  EXPECT_EQ(estimate.effective_volume_ahead_units, 0);
  EXPECT_DOUBLE_EQ(estimate.fill_probability, 1.0);
}

TEST(QueuePositionEstimator, ZeroObservedVolumeAheadReportsCertainFillEdgeCase) {
  const auto estimate = QueuePositionEstimator::estimate(0, 0.2, 0);
  EXPECT_DOUBLE_EQ(estimate.fill_probability, 1.0);
}

}  // namespace
