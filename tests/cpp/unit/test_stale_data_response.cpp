#include <gtest/gtest.h>

#include "cpp/participant/app/fault_scenario.hpp"

/// AEGIS-060: a `kDelayed`-annotated M2 fault stream drives the participant
/// book stale. The M2 injector (`replay::DeterministicFaultInjector`) is
/// consumed unmodified; the response -- staleness detection against the
/// delayed delivery time -- is what M3 adds (ADR-0021).
///
/// Staleness is checked against each record's *scheduled* time, before any
/// delay -- checking only after a delayed message finally arrives would
/// always see a book "just updated" (age zero), which is why every scenario
/// below needs at least one message the delay pushes past, not just one
/// delayed message in isolation.
namespace {

using aegis::common::Duration;
using aegis::events::exchange::Side;
using aegis::events::market_data::BookDeltaEvent;
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

TEST(StaleDataResponse, DelayedFaultDrivesTheBookStaleBeforeTheLateMessageArrives) {
  // Record 0 arrives on time at t=0, establishing a baseline. Record 1 is
  // scheduled for t=200 but delayed by 5000ns -- by t=200 (its schedule),
  // the threshold (100ns since t=0) has already been crossed.
  const std::vector<BookDeltaEvent> deltas{make_delta(2, 1, 100), make_delta(3, 2, 105)};
  const std::vector<ReplayEvent> timing{make_timing(0, 0), make_timing(1, 200)};
  const std::vector<FaultRule> rules{
      FaultRule{.target = RecordIndex{1}, .kind = FaultKind::kDelayed, .delay = Duration{5000}}};

  const auto outcome =
      run_fault_scenario(make_initial_snapshot(), deltas, timing, rules,
                         /*recovery_snapshot=*/std::nullopt, /*max_staleness_age=*/Duration{100},
                         /*max_consecutive_faults=*/0);

  EXPECT_TRUE(outcome.went_stale);
  // The delayed delta still applies once it arrives -- staleness is a
  // signal alongside the book, not a refusal to hold state (ADR-0021).
  EXPECT_EQ(outcome.final_best_bid_price_units, 105);
}

TEST(StaleDataResponse, OnTimeDeliveryWithinThresholdIsNotStale) {
  const std::vector<BookDeltaEvent> deltas{make_delta(2, 1, 100), make_delta(3, 2, 105)};
  const std::vector<ReplayEvent> timing{make_timing(0, 0), make_timing(1, 50)};
  const std::vector<FaultRule> rules{};  // No fault: on-time delivery for both.

  const auto outcome = run_fault_scenario(make_initial_snapshot(), deltas, timing, rules,
                                          std::nullopt, Duration{100},
                                          /*max_consecutive_faults=*/0);

  EXPECT_FALSE(outcome.went_stale);
}

TEST(StaleDataResponse, ADelayShorterThanTheThresholdDoesNotGoStale) {
  const std::vector<BookDeltaEvent> deltas{make_delta(2, 1, 100), make_delta(3, 2, 105)};
  const std::vector<ReplayEvent> timing{make_timing(0, 0), make_timing(1, 50)};
  const std::vector<FaultRule> rules{
      FaultRule{.target = RecordIndex{1}, .kind = FaultKind::kDelayed, .delay = Duration{10}}};

  const auto outcome = run_fault_scenario(make_initial_snapshot(), deltas, timing, rules,
                                          std::nullopt, Duration{1000},
                                          /*max_consecutive_faults=*/0);

  EXPECT_FALSE(outcome.went_stale);
}

}  // namespace
