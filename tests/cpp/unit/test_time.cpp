#include <type_traits>

#include <gtest/gtest.h>

#include "cpp/common/clock.hpp"
#include "cpp/common/time.hpp"

namespace {

using aegis::common::AckTime;
using aegis::common::Duration;
using aegis::common::EventTime;
using aegis::common::ManualClock;
using aegis::common::ManualSteadyClock;
using aegis::common::MonotonicTime;
using aegis::common::ReceiveTime;
using aegis::common::SubmitTime;

// ---------------------------------------------------------------------------
// The central guarantee of ADR-0002, asserted at compile time.
//
// All seven domains are 64-bit integers underneath, so nothing about their
// representation prevents subtracting a monotonic reading from an exchange
// timestamp — and the result would be a plausible-looking latency figure rather
// than an error. These assertions are the enforcement; a runtime test could not
// express "this must not compile".
// ---------------------------------------------------------------------------

template <typename A, typename B>
concept Subtractable = requires(A a, B b) { a - b; };

static_assert(Subtractable<AckTime, AckTime>, "same-domain subtraction must work");
static_assert(
    !Subtractable<AckTime, MonotonicTime>,
    "latency must never be derived by mixing a wall-clock stamp with a monotonic reading");
static_assert(!Subtractable<EventTime, ReceiveTime>,
              "event and receive stamps come from different clocks and may not be subtracted");
static_assert(!Subtractable<SubmitTime, aegis::common::ExchangeTime>,
              "submit and exchange stamps come from different clocks");

static_assert(!std::is_convertible_v<EventTime, ReceiveTime>,
              "domains must not convert implicitly into one another");
static_assert(!std::is_constructible_v<EventTime, ReceiveTime>,
              "crossing a domain must be an explicit, visible act");

// A monotonic reading is meaningful only inside one process run, so it must not
// reach a persisted or hashed record (AEGIS-005).
template <typename T>
concept Serializable = requires(T t) { aegis::common::serialize_nanos(t); };

static_assert(Serializable<EventTime>);
static_assert(Serializable<AckTime>);
static_assert(!Serializable<MonotonicTime>,
              "serialising a monotonic reading would replay to a different value");

TEST(Timestamp, SameDomainDifferenceYieldsADuration) {
  const EventTime first{1'000};
  const EventTime second{1'500};
  EXPECT_EQ((second - first).nanos(), 500);
  EXPECT_EQ((first - second).nanos(), -500);
}

TEST(Timestamp, OrdersWithinItsDomain) {
  EXPECT_LT(EventTime{1}, EventTime{2});
  EXPECT_EQ(EventTime{7}, EventTime{7});
  EXPECT_GT(EventTime{9}, EventTime{8});
}

TEST(Timestamp, ShiftsByADuration) {
  const ReceiveTime base{10'000};
  EXPECT_EQ((base + aegis::common::micros(5)).nanos(), 15'000);
  EXPECT_EQ((base - aegis::common::micros(5)).nanos(), 5'000);
}

TEST(Duration, IsSignedSoBackwardsStepsStayVisible) {
  // Unsigned arithmetic would turn a clock stepping backwards into a duration of
  // roughly 18 quintillion nanoseconds, which is precisely the anomaly replay
  // validation has to detect.
  const Duration backwards = EventTime{5} - EventTime{9};
  EXPECT_LT(backwards.nanos(), 0);
  EXPECT_EQ(backwards.nanos(), -4);
}

TEST(Duration, ConvertsToCoarserUnits) {
  const Duration one_ms = aegis::common::millis(1);
  EXPECT_EQ(one_ms.nanos(), 1'000'000);
  EXPECT_DOUBLE_EQ(one_ms.micros(), 1'000.0);
  EXPECT_DOUBLE_EQ(one_ms.seconds(), 0.001);
}

TEST(ManualClock, ProducesExactlyTheTimeATestAsksFor) {
  ManualClock clock{1'700'000'000'000'000'000};
  EXPECT_EQ(clock.now_utc(), 1'700'000'000'000'000'000);

  clock.advance(aegis::common::millis(250));
  EXPECT_EQ(clock.now_utc(), 1'700'000'000'250'000'000);

  const auto stamp = clock.stamp<EventTime>();
  EXPECT_EQ(stamp.nanos(), 1'700'000'000'250'000'000);
}

TEST(ManualSteadyClock, NeverMovesBackwards) {
  ManualSteadyClock clock{100};
  clock.advance(Duration{-50});
  EXPECT_EQ(clock.now().nanos(), 100) << "a monotonic clock that can move backwards would make a "
                                         "negative latency indistinguishable from a real bug";

  clock.advance(Duration{25});
  EXPECT_EQ(clock.now().nanos(), 125);
}

TEST(Elapsed, MeasuresBetweenTwoMonotonicReadings) {
  ManualSteadyClock clock{0};
  const MonotonicTime start = clock.now();
  clock.advance(aegis::common::micros(42));
  const MonotonicTime end = clock.now();

  EXPECT_EQ(aegis::common::elapsed(start, end).nanos(), 42'000);
}

}  // namespace
