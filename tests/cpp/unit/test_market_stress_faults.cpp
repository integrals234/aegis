#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/replay/fault_injection.hpp"
#include "cpp/replay/replay_event.hpp"

// ---------------------------------------------------------------------------
// M2 slice 12 -- market stress (AEGIS-062) and execution stress (AEGIS-063)
// fault kinds. Same DeterministicFaultInjector mechanism as slice 11: pure
// annotation, never interpreted here. Response is the registered M5
// residual (risk response for 062, OMS/risk integration for 063).
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

struct StressKindCase {
  std::string name;
  FaultKind kind;
};

class MarketStressFaultTest : public testing::TestWithParam<StressKindCase> {};

TEST_P(MarketStressFaultTest, RecordSurvivesAnnotatedWithTheGivenMagnitude) {
  const auto& [name, kind] = GetParam();
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{1}, .kind = kind, .delay = {}, .magnitude = 42}};
  const auto result = DeterministicFaultInjector::apply(sample(), rules);

  ASSERT_EQ(result.events.size(), 3U);
  EXPECT_TRUE(result.dropped.empty());  // stress kinds annotate; they never drop

  const auto& [event, annotation] = result.events[1];
  EXPECT_EQ(event.record_index.value(), 1U);
  EXPECT_EQ(event.source_sequence.value(), 1U);  // untouched
  ASSERT_TRUE(annotation.has_value());
  const auto a = annotation.value();  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(a.kind, kind);
  EXPECT_EQ(a.magnitude, 42U);
}

INSTANTIATE_TEST_SUITE_P(
    AllSevenStressKinds, MarketStressFaultTest,
    testing::Values(StressKindCase{"SpreadWidening", FaultKind::kSpreadWidening},
                    StressKindCase{"VolatilitySpike", FaultKind::kVolatilitySpike},
                    StressKindCase{"LiquidityVanish", FaultKind::kLiquidityVanish},
                    StressKindCase{"Rejection", FaultKind::kRejection},
                    StressKindCase{"PartialFill", FaultKind::kPartialFill},
                    StressKindCase{"Backpressure", FaultKind::kBackpressure}),
    [](const testing::TestParamInfo<StressKindCase>& info) { return info.param.name; });

TEST(MarketStressFault, LatencySpikeUsesTheDelayFieldNotMagnitude) {
  const std::vector<FaultRule> rules{{.target = RecordIndex{0},
                                      .kind = FaultKind::kLatencySpike,
                                      .delay = Duration{2500},
                                      .magnitude = 0}};
  const auto result = DeterministicFaultInjector::apply(sample(), rules);
  ASSERT_EQ(result.events.size(), 3U);
  const auto& [event, annotation] = result.events[0];
  ASSERT_TRUE(annotation.has_value());
  const auto a = annotation.value();  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(a.kind, FaultKind::kLatencySpike);
  EXPECT_EQ(a.delay, Duration{2500});
}

TEST(MarketStressFault, StressAndCoreFaultsComposeOverDifferentRecords) {
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{0}, .kind = FaultKind::kSpreadWidening, .delay = {}, .magnitude = 10},
      {.target = RecordIndex{1}, .kind = FaultKind::kMissing, .delay = {}, .magnitude = 0},
      {.target = RecordIndex{2}, .kind = FaultKind::kRejection, .delay = {}, .magnitude = 1},
  };
  const auto result = DeterministicFaultInjector::apply(sample(), rules);

  ASSERT_EQ(result.events.size(), 2U);
  ASSERT_EQ(result.dropped.size(), 1U);
  EXPECT_EQ(result.dropped[0].record_index.value(), 1U);
  EXPECT_EQ(result.events[0].first.record_index.value(), 0U);
  EXPECT_EQ(result.events[1].first.record_index.value(), 2U);
}

TEST(MarketStressFault, IsDeterministicAcrossRepeatedCalls) {
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{0}, .kind = FaultKind::kVolatilitySpike, .delay = {}, .magnitude = 5},
      {.target = RecordIndex{1}, .kind = FaultKind::kLiquidityVanish, .delay = {}, .magnitude = 99},
      {.target = RecordIndex{2}, .kind = FaultKind::kPartialFill, .delay = {}, .magnitude = 5000},
  };
  const auto first = DeterministicFaultInjector::apply(sample(), rules);
  const auto second = DeterministicFaultInjector::apply(sample(), rules);

  ASSERT_EQ(first.events.size(), second.events.size());
  for (std::size_t i = 0; i < first.events.size(); ++i) {
    const auto& first_annotation = first.events[i].second;
    const auto& second_annotation = second.events[i].second;
    ASSERT_EQ(first_annotation.has_value(), second_annotation.has_value());
    if (!first_annotation.has_value()) {
      continue;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) - guarded above
    EXPECT_EQ(first_annotation->kind, second_annotation->kind);
    const auto first_magnitude =
        first_annotation->magnitude;  // NOLINT(bugprone-unchecked-optional-access)
    const auto second_magnitude =
        second_annotation->magnitude;  // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_EQ(first_magnitude, second_magnitude);
  }
}

}  // namespace
