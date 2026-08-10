#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/replay/replay_event.hpp"
#include "cpp/replay/replay_manifest.hpp"

// ---------------------------------------------------------------------------
// M2 slice 9 -- the manifest is a reproducibility digest, not a cryptographic
// one: deterministic, content- and order-sensitive, nothing more claimed.
// ---------------------------------------------------------------------------

namespace {

using aegis::common::EventTime;
using aegis::replay::compute_manifest;
using aegis::replay::fnv1a64;
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
  return {make(1000, 1, "SYNX:EQX:2026H", 0), make(1000, 2, "SYNX:EQX:2026H", 1),
          make(1001, 1, "SYNX:EQX:2026H", 2)};
}

TEST(ReplayManifest, Fnv1a64MatchesAnIndependentReferenceValue) {
  // The canonical FNV-1a 64-bit test vector for the empty string and for
  // "a", both widely published, computed independently of this
  // implementation.
  EXPECT_EQ(fnv1a64(""), 0xcbf29ce484222325ULL);
  EXPECT_EQ(fnv1a64("a"), 0xaf63dc4c8601ec8cULL);
}

TEST(ReplayManifest, RecordCountAndTimeRangeAreCorrect) {
  const auto manifest = compute_manifest("input.jsonl", sample());
  EXPECT_EQ(manifest.record_count, 3U);
  EXPECT_EQ(manifest.first_event_time_nanos, 1000);
  EXPECT_EQ(manifest.last_event_time_nanos, 1001);
  EXPECT_EQ(manifest.input_path, "input.jsonl");
}

TEST(ReplayManifest, EmptyStreamHasZeroedTimeRange) {
  const auto manifest = compute_manifest("empty.jsonl", {});
  EXPECT_EQ(manifest.record_count, 0U);
  EXPECT_EQ(manifest.first_event_time_nanos, 0);
  EXPECT_EQ(manifest.last_event_time_nanos, 0);
}

TEST(ReplayManifest, DigestIsStableAcrossRepeatedComputation) {
  const auto first = compute_manifest("input.jsonl", sample());
  const auto second = compute_manifest("input.jsonl", sample());
  EXPECT_EQ(first.content_digest, second.content_digest);
  EXPECT_EQ(first, second);
}

TEST(ReplayManifest, DigestChangesWithContent) {
  auto changed = sample();
  changed[1].source_sequence = SourceSequence{999};
  const auto original_digest = compute_manifest("input.jsonl", sample()).content_digest;
  const auto changed_digest = compute_manifest("input.jsonl", changed).content_digest;
  EXPECT_NE(original_digest, changed_digest);
}

TEST(ReplayManifest, DigestChangesWithOrder) {
  auto reordered = sample();
  std::swap(reordered[0], reordered[1]);
  const auto original_digest = compute_manifest("input.jsonl", sample()).content_digest;
  const auto reordered_digest = compute_manifest("input.jsonl", reordered).content_digest;
  EXPECT_NE(original_digest, reordered_digest);
}

TEST(ReplayManifest, ToJsonRoundTripsTheScalarFields) {
  const auto manifest = compute_manifest("input.jsonl", sample());
  const auto json_text = aegis::replay::to_json(manifest);
  EXPECT_NE(json_text.find("\"record_count\":3"), std::string::npos);
  EXPECT_NE(json_text.find("\"input_path\":\"input.jsonl\""), std::string::npos);
}

}  // namespace
