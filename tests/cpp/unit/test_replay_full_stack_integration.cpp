#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/common/time.hpp"
#include "cpp/replay/fault_injection.hpp"
#include "cpp/replay/pacing.hpp"
#include "cpp/replay/replay_engine.hpp"
#include "cpp/replay/replay_event.hpp"
#include "cpp/replay/replay_manifest.hpp"
#include "cpp/replay/replay_stream.hpp"
#include "cpp/replay/virtual_clock.hpp"

// ---------------------------------------------------------------------------
// M2 slice 8-13 light checkpoint -- one deterministic test spanning every
// replay stage slices 9-13 built: a persisted canonical input (the
// equivalent of what ingestion would have written to disk) is loaded and
// validated (replay_stream -- "ingestion" boundary), drained through
// ReplayEngine with an accelerated pacing policy (replay + pacing), fault-
// annotated over the same validated events (fault_injection), and the
// emitted order is checked against the same canonical_less the C++/Python
// bindings symmetry test exercises through sort_canonical
// (tests/integration/test_bindings_roundtrip.py -- feed/bindings). A second,
// fully independent run over the same input proves the whole chain -- not
// just any one stage -- reproduces identically.
// ---------------------------------------------------------------------------

namespace {

using aegis::common::Duration;
using aegis::replay::AcceleratedPacing;
using aegis::replay::canonical_less;
using aegis::replay::compute_manifest;
using aegis::replay::DeterministicFaultInjector;
using aegis::replay::FaultInjectionResult;
using aegis::replay::FaultKind;
using aegis::replay::FaultRule;
using aegis::replay::load_replay_stream;
using aegis::replay::RecordIndex;
using aegis::replay::ReplayEngine;
using aegis::replay::ReplayEvent;
using aegis::replay::VirtualClock;

std::filesystem::path write_fixture() {
  const auto path =
      std::filesystem::temp_directory_path() / "aegis_replay_full_stack_integration.jsonl";
  std::ofstream file(path);
  file << "{\"event_time_ns\":1000,\"source_sequence\":1,\"contract_symbol\":\"SYNX:EQX:2026H\","
          "\"record_index\":0}\n"
          "{\"event_time_ns\":1000,\"source_sequence\":2,\"contract_symbol\":\"SYNX:EQX:2026H\","
          "\"record_index\":1}\n"
          "{\"event_time_ns\":1500,\"source_sequence\":1,\"contract_symbol\":\"SYNX:EQX:2026H\","
          "\"record_index\":2}\n"
          "{\"event_time_ns\":2000,\"source_sequence\":1,\"contract_symbol\":\"SYNX:EQX:2026H\","
          "\"record_index\":3}\n";
  file.close();
  return path;
}

struct FullStackOutcome {
  std::vector<ReplayEvent> emitted;
  std::vector<Duration> waits;
  std::uint64_t manifest_digest{0};
  FaultInjectionResult faulted;
};

FullStackOutcome run_full_stack(const std::string& path) {
  // Stage: replay_stream -- load and validate the persisted canonical input.
  const auto loaded = load_replay_stream(path);
  EXPECT_TRUE(loaded.has_value());
  const auto& events = loaded.value();

  // Stage: replay_manifest -- the reproducibility digest over the loaded,
  // already-canonical order.
  const auto manifest = compute_manifest(path, events);

  // Stage: replay_engine + pacing -- drain with AcceleratedPacing(2.0),
  // collecting each emitted event and its computed (never slept) wait.
  VirtualClock clock;
  ReplayEngine engine(events, clock);
  const AcceleratedPacing pacing(2.0);

  FullStackOutcome outcome;
  outcome.manifest_digest = manifest.content_digest;
  while (const auto next = engine.next_with_pacing(pacing)) {
    outcome.emitted.push_back(next->first);
    outcome.waits.push_back(next->second);
  }

  // Stage: fault_injection -- annotate one core (slice 11) and one stress
  // (slice 12) fault kind over the same validated events.
  const std::vector<FaultRule> rules{
      {.target = RecordIndex{1},
       .kind = FaultKind::kDelayed,
       .delay = Duration{500},
       .magnitude = 0},
      {.target = RecordIndex{3}, .kind = FaultKind::kLiquidityVanish, .delay = {}, .magnitude = 7},
  };
  outcome.faulted = DeterministicFaultInjector::apply(events, rules);

  return outcome;
}

TEST(ReplayFullStackIntegration, EveryStageAgreesOnOneCanonicalOrder) {
  const auto path = write_fixture();
  const auto outcome = run_full_stack(path.string());

  // replay_engine's emission order is the same order load_replay_stream
  // validated -- the same order the C++/Python bindings symmetry test
  // proves canonical_less/sort_canonical agree on (feed/bindings boundary).
  ASSERT_EQ(outcome.emitted.size(), 4U);
  for (std::size_t i = 0; i + 1 < outcome.emitted.size(); ++i) {
    EXPECT_TRUE(canonical_less(outcome.emitted[i], outcome.emitted[i + 1]));
  }
  std::vector<std::uint64_t> actual_order;
  actual_order.reserve(outcome.emitted.size());
  for (const auto& event : outcome.emitted) {
    actual_order.push_back(event.record_index.value());
  }
  EXPECT_EQ(actual_order, (std::vector<std::uint64_t>{0, 1, 2, 3}));

  // Pacing computed a wait, never slept -- gaps are 0/500/500ns, so a test
  // that actually slept for these would still be effectively instant; the
  // exact scaled values below are what proves the computation ran at all.
  ASSERT_EQ(outcome.waits.size(), 4U);
  EXPECT_EQ(outcome.waits[0], Duration{0});    // first emission has no predecessor
  EXPECT_EQ(outcome.waits[1], Duration{0});    // (1000-1000)/2
  EXPECT_EQ(outcome.waits[2], Duration{250});  // (1500-1000)/2
  EXPECT_EQ(outcome.waits[3], Duration{250});  // (2000-1500)/2

  // Fault injection annotated the two targeted records and left the other
  // two untouched, without dropping or reordering anything.
  ASSERT_EQ(outcome.faulted.events.size(), 4U);
  EXPECT_TRUE(outcome.faulted.dropped.empty());
  EXPECT_FALSE(outcome.faulted.events[0].second.has_value());
  ASSERT_TRUE(outcome.faulted.events[1].second.has_value());
  const auto delayed =
      outcome.faulted.events[1].second.value();  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(delayed.kind, FaultKind::kDelayed);
  EXPECT_EQ(delayed.delay, Duration{500});
  EXPECT_FALSE(outcome.faulted.events[2].second.has_value());
  ASSERT_TRUE(outcome.faulted.events[3].second.has_value());
  const auto vanish =
      outcome.faulted.events[3].second.value();  // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(vanish.kind, FaultKind::kLiquidityVanish);
  EXPECT_EQ(vanish.magnitude, 7U);

  std::filesystem::remove(path);
}

TEST(ReplayFullStackIntegration, WholeChainIsDeterministicAcrossIndependentRuns) {
  const auto path = write_fixture();
  const auto first = run_full_stack(path.string());
  const auto second = run_full_stack(path.string());

  EXPECT_EQ(first.manifest_digest, second.manifest_digest);
  ASSERT_EQ(first.emitted.size(), second.emitted.size());
  for (std::size_t i = 0; i < first.emitted.size(); ++i) {
    EXPECT_EQ(first.emitted[i].record_index.value(), second.emitted[i].record_index.value());
  }
  EXPECT_EQ(first.waits, second.waits);
  ASSERT_EQ(first.faulted.events.size(), second.faulted.events.size());
  for (std::size_t i = 0; i < first.faulted.events.size(); ++i) {
    EXPECT_EQ(first.faulted.events[i].second.has_value(),
              second.faulted.events[i].second.has_value());
  }

  std::filesystem::remove(path);
}

}  // namespace
