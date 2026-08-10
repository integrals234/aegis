#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/replay/fault_injection.hpp"
#include "cpp/replay/replay_event.hpp"

// ---------------------------------------------------------------------------
// M2 slice 11 -- deterministic fault injection: the mechanism only, no
// stale-data response (AEGIS-060, M3 residual) or recovery (AEGIS-061, M3
// residual). Every fault is explicit, seeded data, never randomness the
// caller has to trust.
// ---------------------------------------------------------------------------

namespace {

using aegis::common::Duration;
using aegis::common::EventTime;
using aegis::replay::DeterministicFaultInjector;
using aegis::replay::FaultKind;
using aegis::replay::FaultRule;
using aegis::replay::RecordIndex;
using aegis::replay::ReplayEvent;
using aegis::replay::SourceSequence;

ReplayEvent make(std::int64_t time, std::uint64_t sequence, std::string symbol,
                 std::uint64_t index) {
  return ReplayEvent{.event_time = EventTime{time},
                     .source_sequence = SourceSequence{sequence},
                     .contract_symbol = std::move(symbol),
                     .record_index = RecordIndex{index}};
}

std::vector<ReplayEvent> sample() {
  return {make(1000, 1, "A", 0), make(1001, 1, "A", 1), make(1002, 1, "A", 2)};
}

TEST(DeterministicFaultInjector, NoRulesLeavesTheStreamUnannotatedAndUnchanged) {
  const auto result = DeterministicFaultInjector::apply(sample(), {});
  ASSERT_EQ(result.events.size(), 3U);
  for (const auto& [event, annotation] : result.events) {
    EXPECT_FALSE(annotation.has_value());
  }
  EXPECT_TRUE(result.dropped.empty());
}

TEST(DeterministicFaultInjector, DelayedRecordIsAnnotatedButNotRemoved) {
  const std::vector<FaultRule> rules{{.target = RecordIndex{1},
                                      .kind = FaultKind::kDelayed,
                                      .delay = Duration{500},
                                      .magnitude = 0}};
  const auto result = DeterministicFaultInjector::apply(sample(), rules);
  ASSERT_EQ(result.events.size(), 3U);
  EXPECT_TRUE(result.dropped.empty());

  const auto& [event, annotation] = result.events[1];
  EXPECT_EQ(event.record_index.value(), 1U);
  ASSERT_TRUE(annotation.has_value());
  const auto a = annotation.value();  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(a.kind, FaultKind::kDelayed);
  EXPECT_EQ(a.delay, Duration{500});
}

TEST(DeterministicFaultInjector, MissingRecordIsRemovedAndAccountedFor) {
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{1}, .kind = FaultKind::kMissing, .delay = {}, .magnitude = 0}};
  const auto result = DeterministicFaultInjector::apply(sample(), rules);

  ASSERT_EQ(result.events.size(), 2U);
  EXPECT_EQ(result.events[0].first.record_index.value(), 0U);
  EXPECT_EQ(result.events[1].first.record_index.value(), 2U);

  ASSERT_EQ(result.dropped.size(), 1U);
  EXPECT_EQ(result.dropped[0].record_index.value(), 1U);
  EXPECT_EQ(result.dropped[0].reason, FaultKind::kMissing);
}

TEST(DeterministicFaultInjector, DuplicatedRecordAppearsTwiceConsecutively) {
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{1}, .kind = FaultKind::kDuplicated, .delay = {}, .magnitude = 0}};
  const auto result = DeterministicFaultInjector::apply(sample(), rules);

  ASSERT_EQ(result.events.size(), 4U);
  EXPECT_EQ(result.events[0].first.record_index.value(), 0U);
  EXPECT_EQ(result.events[1].first.record_index.value(), 1U);
  EXPECT_FALSE(result.events[1].second.has_value());  // the original copy, unannotated
  EXPECT_EQ(result.events[2].first.record_index.value(), 1U);
  ASSERT_TRUE(result.events[2].second.has_value());
  const auto dup = result.events[2].second.value();  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(dup.kind, FaultKind::kDuplicated);       // the injected copy
  EXPECT_EQ(result.events[3].first.record_index.value(), 2U);
  EXPECT_TRUE(result.dropped.empty());
}

TEST(DeterministicFaultInjector, SequenceGapIsAnnotatedWithoutMutatingSourceSequence) {
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{1}, .kind = FaultKind::kSequenceGap, .delay = {}, .magnitude = 7}};
  const auto result = DeterministicFaultInjector::apply(sample(), rules);

  ASSERT_EQ(result.events.size(), 3U);
  const auto& [event, annotation] = result.events[1];
  EXPECT_EQ(event.source_sequence.value(), 1U);  // unchanged -- canonical order never mutated
  ASSERT_TRUE(annotation.has_value());
  const auto a = annotation.value();  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(a.kind, FaultKind::kSequenceGap);
  EXPECT_EQ(a.magnitude, 7U);
}

TEST(DeterministicFaultInjector, MultipleRulesApplyIndependently) {
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{0},
       .kind = FaultKind::kDelayed,
       .delay = Duration{100},
       .magnitude = 0},
      {.target = RecordIndex{2}, .kind = FaultKind::kMissing, .delay = {}, .magnitude = 0},
  };
  const auto result = DeterministicFaultInjector::apply(sample(), rules);

  ASSERT_EQ(result.events.size(), 2U);
  ASSERT_TRUE(result.events[0].second.has_value());
  const auto a = result.events[0].second.value();  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(a.kind, FaultKind::kDelayed);
  EXPECT_FALSE(result.events[1].second.has_value());
  ASSERT_EQ(result.dropped.size(), 1U);
  EXPECT_EQ(result.dropped[0].record_index.value(), 2U);
}

TEST(DeterministicFaultInjector, AmbiguousRuleSetThrows) {
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{1}, .kind = FaultKind::kDelayed, .delay = Duration{1}, .magnitude = 0},
      {.target = RecordIndex{1}, .kind = FaultKind::kMissing, .delay = {}, .magnitude = 0},
  };
  EXPECT_THROW(
      { [[maybe_unused]] const auto result = DeterministicFaultInjector::apply(sample(), rules); },
      std::invalid_argument);
}

TEST(DeterministicFaultInjector, IsDeterministicAcrossRepeatedCalls) {
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{0}, .kind = FaultKind::kDuplicated, .delay = {}, .magnitude = 0},
      {.target = RecordIndex{1}, .kind = FaultKind::kSequenceGap, .delay = {}, .magnitude = 3},
      {.target = RecordIndex{2}, .kind = FaultKind::kMissing, .delay = {}, .magnitude = 0},
  };
  const auto first = DeterministicFaultInjector::apply(sample(), rules);
  const auto second = DeterministicFaultInjector::apply(sample(), rules);

  ASSERT_EQ(first.events.size(), second.events.size());
  for (std::size_t i = 0; i < first.events.size(); ++i) {
    EXPECT_EQ(first.events[i].first.record_index.value(),
              second.events[i].first.record_index.value());
    EXPECT_EQ(first.events[i].second.has_value(), second.events[i].second.has_value());
  }
  ASSERT_EQ(first.dropped.size(), second.dropped.size());
  for (std::size_t i = 0; i < first.dropped.size(); ++i) {
    EXPECT_EQ(first.dropped[i].record_index.value(), second.dropped[i].record_index.value());
  }
}

}  // namespace
