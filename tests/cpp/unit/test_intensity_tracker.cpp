#include <cstddef>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/participant/app/intensity_tracker.hpp"

/// AEGIS-074: trade/cancellation intensity. Event/count extraction
/// (recognising a decoded trade or cancellation) and the generic
/// cpp-statistics rate estimator meet only in `IntensityTracker`
/// (ADR-0020) -- this test proves the composition, not either half alone
/// (`test_rolling_rate.cpp` already proves the estimator's own
/// online/offline equivalence).
namespace {

using aegis::events::Envelope;
using aegis::events::MessageType;
using aegis::events::exchange::OrderTerminatedEvent;
using aegis::events::exchange::TerminationReason;
using aegis::events::exchange::TradeEvent;
using aegis::participant::app::IntensityTracker;
using aegis::participant::feed::FeedHandler;

Envelope frame(MessageType type, std::vector<std::byte> payload) {
  Envelope envelope;
  envelope.message_type = type;
  envelope.payload = std::move(payload);
  return envelope;
}

TEST(IntensityTracker, CountsDecodedTradesOnly) {
  FeedHandler handler;
  IntensityTracker tracker(aegis::common::Duration{1'000'000'000});

  const auto trade = handler.decode(frame(MessageType::kTrade, encode(TradeEvent{})));
  tracker.observe(trade, /*now_nanos=*/0);
  tracker.observe(trade, /*now_nanos=*/100);

  EXPECT_EQ(tracker.trade_count(), 2U);
  EXPECT_EQ(tracker.cancellation_count(), 0U);
}

TEST(IntensityTracker, CountsCanceledTerminationsButNotOtherReasons) {
  FeedHandler handler;
  IntensityTracker tracker(aegis::common::Duration{1'000'000'000});

  const auto canceled =
      handler.decode(frame(MessageType::kOrderTerminated,
                           encode(OrderTerminatedEvent{.reason = TerminationReason::kCanceled})));
  const auto filled =
      handler.decode(frame(MessageType::kOrderTerminated,
                           encode(OrderTerminatedEvent{.reason = TerminationReason::kFilled})));

  tracker.observe(canceled, 0);
  tracker.observe(filled, 100);  // Not a cancellation: does not count.
  tracker.observe(canceled, 200);

  EXPECT_EQ(tracker.cancellation_count(), 2U);
  EXPECT_EQ(tracker.trade_count(), 0U);
}

TEST(IntensityTracker, UnhandledAndBookMessagesAreNoOps) {
  FeedHandler handler;
  IntensityTracker tracker(aegis::common::Duration{1'000'000'000});

  const auto unhandled = handler.decode(frame(MessageType::kUnspecified, {}));
  tracker.observe(unhandled, 0);

  EXPECT_EQ(tracker.trade_count(), 0U);
  EXPECT_EQ(tracker.cancellation_count(), 0U);
  EXPECT_EQ(tracker.trade_rate_per_second(), 0.0);
  EXPECT_EQ(tracker.cancellation_rate_per_second(), 0.0);
}

}  // namespace
