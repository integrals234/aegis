#include <gtest/gtest.h>

#include "cpp/participant/feed_handler/sequence_tracker.hpp"

namespace {

using aegis::participant::feed::SequenceDiagnostic;
using aegis::participant::feed::SequenceTracker;

TEST(SequenceTracker, FirstObservationIsAlwaysOk) {
  SequenceTracker tracker;
  const auto result = tracker.observe(100);
  EXPECT_EQ(result.diagnostic, SequenceDiagnostic::kOk);
  EXPECT_EQ(tracker.last_sequence(), 100U);
}

TEST(SequenceTracker, ConsecutiveObservationIsOk) {
  SequenceTracker tracker;
  static_cast<void>(tracker.observe(1));
  const auto result = tracker.observe(2);
  EXPECT_EQ(result.diagnostic, SequenceDiagnostic::kOk);
  EXPECT_EQ(tracker.consecutive_faults(), 0U);
}

TEST(SequenceTracker, SkippingAheadIsAGap) {
  SequenceTracker tracker;
  static_cast<void>(tracker.observe(5));
  const auto result = tracker.observe(8);
  EXPECT_EQ(result.diagnostic, SequenceDiagnostic::kGap);
  EXPECT_EQ(result.expected_sequence, 6U);
  EXPECT_EQ(tracker.consecutive_faults(), 1U);
}

TEST(SequenceTracker, RepeatingTheSameSequenceIsADuplicate) {
  SequenceTracker tracker;
  static_cast<void>(tracker.observe(5));
  const auto result = tracker.observe(5);
  EXPECT_EQ(result.diagnostic, SequenceDiagnostic::kDuplicate);
}

TEST(SequenceTracker, GoingBackwardIsAReset) {
  SequenceTracker tracker;
  static_cast<void>(tracker.observe(10));
  const auto result = tracker.observe(3);
  EXPECT_EQ(result.diagnostic, SequenceDiagnostic::kReset);
}

TEST(SequenceTracker, ConsecutiveFaultsResetOnTheNextOk) {
  SequenceTracker tracker;
  static_cast<void>(tracker.observe(1));
  static_cast<void>(tracker.observe(5));  // gap
  static_cast<void>(tracker.observe(5));  // duplicate
  EXPECT_EQ(tracker.consecutive_faults(), 2U);
  static_cast<void>(tracker.observe(6));  // ok
  EXPECT_EQ(tracker.consecutive_faults(), 0U);
}

TEST(SequenceTracker, ResetClearsState) {
  SequenceTracker tracker;
  static_cast<void>(tracker.observe(10));
  tracker.reset();
  EXPECT_FALSE(tracker.last_sequence().has_value());
  const auto result = tracker.observe(1);
  EXPECT_EQ(result.diagnostic, SequenceDiagnostic::kOk);
}

}  // namespace
