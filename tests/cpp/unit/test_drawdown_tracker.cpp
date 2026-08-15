#include <gtest/gtest.h>

#include "cpp/statistics/drawdown_tracker.hpp"

/// AEGIS-106: "scenario tests pass."
namespace {

using aegis::participant::stats::DrawdownTracker;

TEST(DrawdownTracker, HighWaterMarkTracksThePeakAndDrawdownTracksTheRetreat) {
  DrawdownTracker tracker;
  tracker.push(100.0);
  EXPECT_EQ(tracker.high_water_mark(), 100.0);
  EXPECT_EQ(tracker.current_drawdown(), 0.0);

  tracker.push(120.0);  // New peak.
  EXPECT_EQ(tracker.high_water_mark(), 120.0);
  EXPECT_EQ(tracker.current_drawdown(), 0.0);

  tracker.push(90.0);  // Retreat from the peak.
  EXPECT_EQ(tracker.high_water_mark(), 120.0);
  EXPECT_EQ(tracker.current_drawdown(), 30.0);
  EXPECT_EQ(tracker.max_drawdown(), 30.0);

  tracker.push(100.0);  // Partial recovery: still below the peak.
  EXPECT_EQ(tracker.current_drawdown(), 20.0);
  EXPECT_EQ(tracker.max_drawdown(), 30.0);  // Max drawdown does not shrink.

  tracker.push(80.0);  // A deeper drawdown than before.
  EXPECT_EQ(tracker.current_drawdown(), 40.0);
  EXPECT_EQ(tracker.max_drawdown(), 40.0);
}

TEST(DrawdownTracker, RecoveringPastThePeakStartsANewHighWaterMark) {
  DrawdownTracker tracker;
  tracker.push(100.0);
  tracker.push(80.0);   // Drawdown 20.
  tracker.push(150.0);  // New peak, past the old one.
  EXPECT_EQ(tracker.high_water_mark(), 150.0);
  EXPECT_EQ(tracker.current_drawdown(), 0.0);
  EXPECT_EQ(tracker.max_drawdown(), 20.0);  // Still the largest ever seen.
}

TEST(DrawdownTracker, MeanAndVarianceMatchHandComputedValues) {
  DrawdownTracker tracker;
  for (const double v : {2.0, 4.0, 4.0, 4.0}) {
    tracker.push(v);
  }
  EXPECT_DOUBLE_EQ(tracker.mean(), 3.5);
  // Sample variance (ddof=1) of {2,4,4,4}: mean 3.5, sum sq dev
  // = 2.25+0.25+0.25+0.25 = 3.0, /3 = 1.0.
  EXPECT_DOUBLE_EQ(tracker.variance(), 1.0);
}

TEST(DrawdownTracker, EmptyAndSingleObservationEdgeCases) {
  DrawdownTracker tracker;
  EXPECT_EQ(tracker.count(), 0U);
  EXPECT_EQ(tracker.high_water_mark(), 0.0);
  EXPECT_EQ(tracker.variance(), 0.0);

  tracker.push(42.0);
  EXPECT_EQ(tracker.high_water_mark(), 42.0);
  EXPECT_EQ(tracker.current_drawdown(), 0.0);
  EXPECT_EQ(tracker.variance(), 0.0);  // Fewer than two observations.
}

}  // namespace
