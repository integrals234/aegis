#include <gtest/gtest.h>

#include "cpp/participant/oms/missed_trade_tracker.hpp"

/// AEGIS-117: attribution includes missed-trade statistics.
namespace {

using aegis::participant::oms::MissedTradeTracker;

TEST(MissedTradeTracker, StartsEmpty) {
  MissedTradeTracker tracker;
  EXPECT_EQ(tracker.total_missed_quantity_units(), 0);
  EXPECT_EQ(tracker.missed_order_count(), 0U);
  EXPECT_TRUE(tracker.records().empty());
}

TEST(MissedTradeTracker, ZeroMissedQuantityIsANoOpNotACountedRecord) {
  MissedTradeTracker tracker;
  tracker.record(/*client_order_id=*/1, /*missed_quantity_units=*/0);
  EXPECT_EQ(tracker.missed_order_count(), 0U);
  EXPECT_EQ(tracker.total_missed_quantity_units(), 0);
}

TEST(MissedTradeTracker, NegativeMissedQuantityIsRejectedAsANoOp) {
  MissedTradeTracker tracker;
  tracker.record(1, -5);  // Never a legitimate input; treated as nothing to attribute.
  EXPECT_EQ(tracker.missed_order_count(), 0U);
}

TEST(MissedTradeTracker, OutrightRejectionRecordsTheFullRequestedQuantity) {
  MissedTradeTracker tracker;
  tracker.record(/*client_order_id=*/7, /*missed_quantity_units=*/100);
  ASSERT_EQ(tracker.missed_order_count(), 1U);
  EXPECT_EQ(tracker.total_missed_quantity_units(), 100);
  EXPECT_EQ(tracker.records().front().client_order_id, 7U);
  EXPECT_EQ(tracker.records().front().missed_quantity_units, 100);
}

TEST(MissedTradeTracker, CancelledOrderRecordsOnlyTheUnfilledRemainder) {
  MissedTradeTracker tracker;
  constexpr std::int64_t kOriginal = 100;
  constexpr std::int64_t kFilled = 40;
  tracker.record(/*client_order_id=*/9, kOriginal - kFilled);
  EXPECT_EQ(tracker.total_missed_quantity_units(), 60);
}

TEST(MissedTradeTracker, MultipleRecordsAccumulate) {
  MissedTradeTracker tracker;
  tracker.record(1, 30);
  tracker.record(2, 70);
  tracker.record(3, 0);  // No-op: does not inflate the count.
  EXPECT_EQ(tracker.missed_order_count(), 2U);
  EXPECT_EQ(tracker.total_missed_quantity_units(), 100);
}

}  // namespace
