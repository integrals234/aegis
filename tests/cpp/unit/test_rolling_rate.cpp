#include <vector>

#include <gtest/gtest.h>

#include "cpp/statistics/rolling_rate.hpp"

/// AEGIS-074: "online/offline equivalence tests pass."
namespace {

using aegis::common::Duration;
using aegis::common::Nanos;
using aegis::participant::stats::RollingRate;

/// Trusted offline calculation: for a fixed timestamp sequence, the count in
/// the trailing window as of the *last* timestamp.
std::size_t offline_count_in_window(const std::vector<Nanos>& timestamps, Nanos window_nanos) {
  if (timestamps.empty()) {
    return 0;
  }
  const Nanos now = timestamps.back();
  std::size_t count = 0;
  for (const Nanos timestamp : timestamps) {
    if (now - timestamp <= window_nanos) {
      ++count;
    }
  }
  return count;
}

TEST(RollingRate, MatchesOfflineCountAsEventsStreamIn) {
  const std::vector<Nanos> timestamps{0, 100, 200, 300, 900, 1000, 1050, 3000};
  constexpr Nanos kWindow = 500;
  RollingRate rate(Duration{kWindow});

  std::vector<Nanos> seen;
  for (const Nanos timestamp : timestamps) {
    rate.record_event(timestamp);
    seen.push_back(timestamp);
    EXPECT_EQ(rate.count(), offline_count_in_window(seen, kWindow))
        << "mismatch after event at t=" << timestamp;
  }
}

TEST(RollingRate, EmptyReportsZeroCountAndRate) {
  RollingRate rate(Duration{1000});
  EXPECT_EQ(rate.count(), 0U);
  EXPECT_EQ(rate.rate_per_second(), 0.0);
}

TEST(RollingRate, RatePerSecondMatchesCountOverWindowInSeconds) {
  // A 1-second window with 4 events packed at the end should read ~4/s.
  RollingRate rate(Duration{1'000'000'000});
  for (const Nanos timestamp : {0, 100, 200, 300}) {
    rate.record_event(timestamp);
  }
  EXPECT_EQ(rate.count(), 4U);
  EXPECT_DOUBLE_EQ(rate.rate_per_second(), 4.0);
}

TEST(RollingRate, EventsOutsideTheWindowAreEvicted) {
  RollingRate rate(Duration{100});
  rate.record_event(0);
  rate.record_event(50);
  EXPECT_EQ(rate.count(), 2U);
  rate.record_event(1000);  // Far outside the window: evicts everything before it.
  EXPECT_EQ(rate.count(), 1U);
}

}  // namespace
