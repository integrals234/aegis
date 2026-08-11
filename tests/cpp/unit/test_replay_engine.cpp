#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/replay/replay_engine.hpp"
#include "cpp/replay/replay_event.hpp"
#include "cpp/replay/virtual_clock.hpp"

// ---------------------------------------------------------------------------
// M2 slice 9 -- the engine drains a validated stream through a clock, with
// cursor/resume reproducing the tail of an uninterrupted run exactly.
// ---------------------------------------------------------------------------

namespace {

using aegis::common::EventTime;
using aegis::replay::RecordIndex;
using aegis::replay::ReplayEngine;
using aegis::replay::ReplayEvent;
using aegis::replay::SourceSequence;
using aegis::replay::VirtualClock;

ReplayEvent make(std::int64_t time, std::uint64_t sequence, std::string symbol,
                 std::uint64_t index) {
  return ReplayEvent{.event_time = EventTime{time},
                     .source_sequence = SourceSequence{sequence},
                     .contract_symbol = std::move(symbol),
                     .record_index = RecordIndex{index}};
}

std::vector<ReplayEvent> sample() {
  return {
      make(1000, 1, "A", 0),
      make(1000, 2, "A", 1),  // same event_time_ns as the one above -- a group
      make(1001, 1, "A", 2),
      make(1002, 1, "A", 3),
  };
}

TEST(ReplayEngine, DrainsEveryRecordInCanonicalOrder) {
  VirtualClock clock;
  ReplayEngine engine(sample(), clock);
  std::vector<RecordIndex> seen;
  while (auto event = engine.next()) {
    seen.push_back(event->record_index);
  }
  ASSERT_EQ(seen.size(), 4U);
  EXPECT_EQ(seen[0].value(), 0U);
  EXPECT_EQ(seen[3].value(), 3U);
}

TEST(ReplayEngine, AdvancesTheClockToEachEventsOwnTime) {
  VirtualClock clock;
  ReplayEngine engine(sample(), clock);
  [[maybe_unused]] const auto step1 = engine.next();
  EXPECT_EQ(clock.now_utc(), 1000);
  [[maybe_unused]] const auto step2 = engine.next();
  EXPECT_EQ(clock.now_utc(), 1000);
  [[maybe_unused]] const auto step3 = engine.next();
  EXPECT_EQ(clock.now_utc(), 1001);
  [[maybe_unused]] const auto step4 = engine.next();
  EXPECT_EQ(clock.now_utc(), 1002);
}

TEST(ReplayEngine, NextReturnsNulloptOnceExhausted) {
  VirtualClock clock;
  ReplayEngine engine({make(1, 1, "A", 0)}, clock);
  EXPECT_TRUE(engine.next().has_value());
  EXPECT_FALSE(engine.next().has_value());
  EXPECT_FALSE(engine.has_next());
}

TEST(ReplayEngine, NextGroupEmitsEverySameTimestampRecordAsOneStep) {
  VirtualClock clock;
  ReplayEngine engine(sample(), clock);
  const auto group = engine.next_group();
  ASSERT_EQ(group.size(), 2U);
  EXPECT_EQ(group[0].record_index.value(), 0U);
  EXPECT_EQ(group[1].record_index.value(), 1U);
  EXPECT_EQ(clock.now_utc(), 1000);

  const auto second_group = engine.next_group();
  ASSERT_EQ(second_group.size(), 1U);
  EXPECT_EQ(second_group[0].record_index.value(), 2U);
}

TEST(ReplayEngine, NextGroupOnExhaustedStreamIsEmpty) {
  VirtualClock clock;
  ReplayEngine engine({}, clock);
  EXPECT_TRUE(engine.next_group().empty());
}

TEST(ReplayEngine, CursorIsNulloptBeforeAnyEmission) {
  VirtualClock clock;
  ReplayEngine engine(sample(), clock);
  EXPECT_FALSE(engine.cursor().has_value());
}

TEST(ReplayEngine, CursorTracksTheLastEmittedRecordIndex) {
  VirtualClock clock;
  ReplayEngine engine(sample(), clock);
  [[maybe_unused]] const auto step1 = engine.next();
  [[maybe_unused]] const auto step2 = engine.next();
  const auto cursor = engine.cursor();
  ASSERT_TRUE(cursor.has_value());
  EXPECT_EQ(cursor->value(), 1U);  // NOLINT(bugprone-unchecked-optional-access) - guarded above
}

TEST(ReplayEngine, ResumeFromReproducesTheTailOfAnUninterruptedRun) {
  VirtualClock full_clock;
  ReplayEngine full_run(sample(), full_clock);
  std::vector<RecordIndex> full_tail;
  // Emit record_index 0, then stop -- simulate an interruption.
  [[maybe_unused]] const auto first_step = full_run.next();
  while (auto event = full_run.next()) {
    full_tail.push_back(event->record_index);
  }

  VirtualClock resumed_clock;
  ReplayEngine resumed(sample(), resumed_clock);
  resumed.resume_from(RecordIndex{0});
  std::vector<RecordIndex> resumed_tail;
  while (auto event = resumed.next()) {
    resumed_tail.push_back(event->record_index);
  }

  ASSERT_EQ(full_tail.size(), resumed_tail.size());
  for (std::size_t i = 0; i < full_tail.size(); ++i) {
    EXPECT_EQ(full_tail[i].value(), resumed_tail[i].value());
  }
}

TEST(ReplayEngine, ResumeFromUnknownRecordIndexThrows) {
  VirtualClock clock;
  ReplayEngine engine(sample(), clock);
  EXPECT_THROW(engine.resume_from(RecordIndex{9999}), std::invalid_argument);
}

}  // namespace
