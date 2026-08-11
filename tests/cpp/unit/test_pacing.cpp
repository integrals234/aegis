#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/replay/pacing.hpp"
#include "cpp/replay/replay_engine.hpp"
#include "cpp/replay/replay_event.hpp"
#include "cpp/replay/virtual_clock.hpp"

// ---------------------------------------------------------------------------
// M2 slice 10 -- the four pacing modes compute a virtual wait; none sleeps,
// and all four must emit the identical canonical event sequence.
// ---------------------------------------------------------------------------

namespace {

using aegis::common::Duration;
using aegis::common::EventTime;
using aegis::replay::AcceleratedPacing;
using aegis::replay::FixedRatePacing;
using aegis::replay::OriginalSpeedPacing;
using aegis::replay::RecordIndex;
using aegis::replay::ReplayEngine;
using aegis::replay::ReplayEvent;
using aegis::replay::SourceSequence;
using aegis::replay::StepPacing;
using aegis::replay::VirtualClock;

ReplayEvent make(std::int64_t time, std::uint64_t sequence, std::string symbol,
                 std::uint64_t index) {
  return ReplayEvent{.event_time = EventTime{time},
                     .source_sequence = SourceSequence{sequence},
                     .contract_symbol = std::move(symbol),
                     .record_index = RecordIndex{index}};
}

std::vector<ReplayEvent> sample() {
  return {make(1000, 1, "A", 0), make(1200, 1, "A", 1), make(2000, 1, "A", 2)};
}

// ---------------------------------------------------------- individual modes

TEST(OriginalSpeedPacingTest, WaitEqualsTheOriginalGap) {
  const OriginalSpeedPacing policy;
  const auto wait = policy.wait_before(make(1000, 1, "A", 0), make(1200, 1, "A", 1));
  EXPECT_EQ(wait, Duration{200});
}

TEST(AcceleratedPacingTest, WaitIsTheOriginalGapDividedByTheMultiplier) {
  const AcceleratedPacing policy{2.0};
  const auto wait = policy.wait_before(make(1000, 1, "A", 0), make(1200, 1, "A", 1));
  EXPECT_EQ(wait, Duration{100});
}

TEST(AcceleratedPacingTest, RejectsNonPositiveMultiplier) {
  EXPECT_THROW(AcceleratedPacing(0.0), std::invalid_argument);
  EXPECT_THROW(AcceleratedPacing(-1.0), std::invalid_argument);
}

TEST(FixedRatePacingTest, WaitIsAlwaysTheConfiguredIntervalRegardlessOfOriginalGap) {
  const FixedRatePacing policy{Duration{50}};
  EXPECT_EQ(policy.wait_before(make(1000, 1, "A", 0), make(1200, 1, "A", 1)), Duration{50});
  EXPECT_EQ(policy.wait_before(make(1000, 1, "A", 0), make(5000, 1, "A", 1)), Duration{50});
}

TEST(FixedRatePacingTest, RejectsNegativeInterval) {
  EXPECT_THROW(FixedRatePacing(Duration{-1}), std::invalid_argument);
}

TEST(StepPacingTest, WaitIsAlwaysZero) {
  const StepPacing policy;
  EXPECT_EQ(policy.wait_before(make(1000, 1, "A", 0), make(5000, 1, "A", 1)), Duration{0});
}

// --------------------------------------------------- engine integration

TEST(ReplayEngineWithPacing, FirstEmissionHasZeroWaitRegardlessOfPolicy) {
  VirtualClock clock;
  ReplayEngine engine(sample(), clock);
  const OriginalSpeedPacing policy;
  const auto step = engine.next_with_pacing(policy);
  ASSERT_TRUE(step.has_value());
  const auto w = step.value().second;  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(w, Duration{0});
}

TEST(ReplayEngineWithPacing, OriginalSpeedComputesTheRealGapsAfterTheFirst) {
  VirtualClock clock;
  ReplayEngine engine(sample(), clock);
  const OriginalSpeedPacing policy;
  [[maybe_unused]] const auto first = engine.next_with_pacing(policy);
  const auto second = engine.next_with_pacing(policy);
  ASSERT_TRUE(second.has_value());
  const auto w2 = second.value().second;  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(w2, Duration{200});           // 1200 - 1000

  const auto third = engine.next_with_pacing(policy);
  ASSERT_TRUE(third.has_value());
  const auto w3 = third.value().second;  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(w3, Duration{800});          // 2000 - 1200
}

TEST(ReplayEngineWithPacing, NextWithPacingReturnsNulloptOnceExhausted) {
  VirtualClock clock;
  ReplayEngine engine({make(1, 1, "A", 0)}, clock);
  const StepPacing policy;
  ASSERT_TRUE(engine.next_with_pacing(policy).has_value());
  EXPECT_FALSE(engine.next_with_pacing(policy).has_value());
}

TEST(ReplayEngineWithPacing, AllFourModesEmitTheIdenticalCanonicalEventSequence) {
  const OriginalSpeedPacing original;
  const AcceleratedPacing accelerated{3.0};
  const FixedRatePacing fixed{Duration{10}};
  const StepPacing step;
  const std::vector<const aegis::replay::PacingPolicy*> policies{&original, &accelerated, &fixed,
                                                                 &step};

  std::vector<std::vector<RecordIndex>> sequences_by_policy;
  for (const auto* policy : policies) {
    VirtualClock clock;
    ReplayEngine engine(sample(), clock);
    std::vector<RecordIndex> sequence;
    while (auto step_result = engine.next_with_pacing(*policy)) {
      sequence.push_back(step_result->first.record_index);
    }
    sequences_by_policy.push_back(std::move(sequence));
  }

  for (const auto& sequence : sequences_by_policy) {
    ASSERT_EQ(sequence.size(), sequences_by_policy.front().size());
    for (std::size_t i = 0; i < sequence.size(); ++i) {
      EXPECT_EQ(sequence[i].value(), sequences_by_policy.front()[i].value());
    }
  }
}

}  // namespace
