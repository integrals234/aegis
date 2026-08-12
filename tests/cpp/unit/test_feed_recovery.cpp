#include <gtest/gtest.h>

#include "cpp/participant/app/fault_scenario.hpp"

/// AEGIS-061: feed recovery covers each of M2's missing/duplicate/
/// sequence-gap faults. The M2 injector is consumed unmodified
/// (`replay::DeterministicFaultInjector`); recovery -- buffer, re-base on a
/// fresh snapshot, replay what survived -- is what M3 adds (ADR-0021),
/// reusing the exact mechanism AEGIS-070 needs.
namespace {

using aegis::common::Duration;
using aegis::events::exchange::Side;
using aegis::events::market_data::BookDeltaEvent;
using aegis::events::market_data::BookLevelEntry;
using aegis::events::market_data::BookSnapshotEvent;
using aegis::events::market_data::DeltaKind;
using aegis::participant::app::run_fault_scenario;
using aegis::replay::FaultKind;
using aegis::replay::FaultRule;
using aegis::replay::RecordIndex;
using aegis::replay::ReplayEvent;
using aegis::replay::SourceSequence;

BookSnapshotEvent make_initial_snapshot() {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = 1;
  snapshot.md_sequence = 1;
  return snapshot;
}

BookDeltaEvent make_delta(std::uint64_t md_sequence, std::uint64_t order_id,
                          std::int64_t price_units) {
  BookDeltaEvent delta;
  delta.instrument_id = 1;
  delta.md_sequence = md_sequence;
  delta.kind = DeltaKind::kOrderAdded;
  delta.order_id = order_id;
  delta.side = Side::kBuy;
  delta.price_units = price_units;
  delta.quantity_units = 10;
  return delta;
}

ReplayEvent make_timing(std::uint64_t record_index, std::int64_t event_time_nanos) {
  ReplayEvent event;
  event.event_time = aegis::common::EventTime{event_time_nanos};
  event.source_sequence = SourceSequence{record_index};
  event.contract_symbol = "TEST";
  event.record_index = RecordIndex{record_index};
  return event;
}

// Priced below every delta fixture's price (100-103) so a replayed delta,
// not the recovery snapshot's own level, ends up best bid where a test
// expects that -- the snapshot's presence is still visible via quantity_at.
BookSnapshotEvent make_recovery_snapshot(std::uint64_t md_sequence) {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = 1;
  snapshot.md_sequence = md_sequence;
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 50, .quantity_units = 1, .order_id = 999});
  return snapshot;
}

TEST(FeedRecovery, MissingFaultTriggersBufferRebaseReplayRecovery) {
  // Records 0 and 2 arrive; record 1 (kMissing) never does. The gap is
  // between the surviving md_sequence values themselves -- 2, then 4.
  const std::vector<BookDeltaEvent> deltas{make_delta(2, 1, 100), make_delta(3, 2, 101),
                                           make_delta(4, 3, 102)};
  const std::vector<ReplayEvent> timing{make_timing(0, 0), make_timing(1, 10), make_timing(2, 20)};
  const std::vector<FaultRule> rules{FaultRule{
      .target = RecordIndex{1}, .kind = FaultKind::kMissing, .delay = {}, .magnitude = 0}};

  const auto outcome =
      run_fault_scenario(make_initial_snapshot(), deltas, timing, rules,
                         /*recovery_snapshot=*/make_recovery_snapshot(3), Duration{1'000'000'000},
                         /*max_consecutive_faults=*/0);

  ASSERT_TRUE(outcome.recovered);
  // The recovery snapshot's own level, plus record 2's delta (md_sequence 4
  // > the snapshot's 3, so it replays) at a higher price than the snapshot's.
  EXPECT_EQ(outcome.final_best_bid_price_units, 102);
  EXPECT_EQ(outcome.final_md_sequence, 4U);
}

TEST(FeedRecovery, MissingFaultWithNoRecoverySnapshotLeavesRecoveryIncomplete) {
  // No silent gap tolerance: without a snapshot to re-base on, the book
  // stays in recovery rather than guessing.
  const std::vector<BookDeltaEvent> deltas{make_delta(2, 1, 100), make_delta(3, 2, 101),
                                           make_delta(4, 3, 102)};
  const std::vector<ReplayEvent> timing{make_timing(0, 0), make_timing(1, 10), make_timing(2, 20)};
  const std::vector<FaultRule> rules{FaultRule{
      .target = RecordIndex{1}, .kind = FaultKind::kMissing, .delay = {}, .magnitude = 0}};

  const auto outcome =
      run_fault_scenario(make_initial_snapshot(), deltas, timing, rules,
                         /*recovery_snapshot=*/std::nullopt, Duration{1'000'000'000},
                         /*max_consecutive_faults=*/0);

  EXPECT_FALSE(outcome.recovered);
}

TEST(FeedRecovery, DuplicatedFaultIsDetectedAndSkippedNotReapplied) {
  const std::vector<BookDeltaEvent> deltas{make_delta(2, 1, 100), make_delta(3, 2, 101)};
  const std::vector<ReplayEvent> timing{make_timing(0, 0), make_timing(1, 10)};
  const std::vector<FaultRule> rules{FaultRule{
      .target = RecordIndex{0}, .kind = FaultKind::kDuplicated, .delay = {}, .magnitude = 0}};

  const auto outcome =
      run_fault_scenario(make_initial_snapshot(), deltas, timing, rules, std::nullopt,
                         Duration{1'000'000'000}, /*max_consecutive_faults=*/0);

  // A duplicate never looks like a gap: no recovery needed at all.
  EXPECT_FALSE(outcome.recovered);
  EXPECT_EQ(outcome.final_best_bid_price_units, 101);
  EXPECT_EQ(outcome.final_best_bid_quantity_units, 10);  // Not 20: the repeat did not re-add.
}

TEST(FeedRecovery, SequenceGapFaultTriggersRecovery) {
  // The annotated record's own md_sequence jumps ahead -- the source
  // declaring a gap in its own numbering -- which SequenceTracker detects
  // exactly as it would any other gap (ADR-0021: no special-case handling
  // needed for this fault kind beyond what the generic gap path already does).
  const std::vector<BookDeltaEvent> deltas{make_delta(2, 1, 100), make_delta(10, 2, 103)};
  const std::vector<ReplayEvent> timing{make_timing(0, 0), make_timing(1, 10)};
  const std::vector<FaultRule> rules{FaultRule{
      .target = RecordIndex{1}, .kind = FaultKind::kSequenceGap, .delay = {}, .magnitude = 7}};

  const auto outcome =
      run_fault_scenario(make_initial_snapshot(), deltas, timing, rules,
                         /*recovery_snapshot=*/make_recovery_snapshot(9), Duration{1'000'000'000},
                         /*max_consecutive_faults=*/0);

  ASSERT_TRUE(outcome.recovered);
  EXPECT_EQ(outcome.final_best_bid_price_units, 103);  // md_sequence 10 > snapshot's 9: replayed.
  EXPECT_EQ(outcome.final_md_sequence, 10U);
}

}  // namespace
