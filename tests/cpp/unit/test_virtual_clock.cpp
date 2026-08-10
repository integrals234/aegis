#include <stdexcept>

#include <gtest/gtest.h>

#include "cpp/replay/virtual_clock.hpp"

// ---------------------------------------------------------------------------
// M2 slice 9 -- the virtual clock's determinism is structural: it has no
// code path that reads a system clock, and its only mutator is advance_to.
// ---------------------------------------------------------------------------

namespace {

using aegis::common::EventTime;
using aegis::replay::VirtualClock;

TEST(VirtualClock, StartsAtTheGivenTime) {
  const VirtualClock clock{1000};
  EXPECT_EQ(clock.now_utc(), 1000);
}

TEST(VirtualClock, DefaultsToZero) {
  const VirtualClock clock;
  EXPECT_EQ(clock.now_utc(), 0);
}

TEST(VirtualClock, AdvancesForwardToTheGivenEventTime) {
  VirtualClock clock{100};
  clock.advance_to(EventTime{500});
  EXPECT_EQ(clock.now_utc(), 500);
}

TEST(VirtualClock, AdvancingToTheSameTimeIsAllowed) {
  VirtualClock clock{500};
  clock.advance_to(EventTime{500});
  EXPECT_EQ(clock.now_utc(), 500);
}

TEST(VirtualClock, RefusesToMoveBackward) {
  VirtualClock clock{500};
  EXPECT_THROW(clock.advance_to(EventTime{499}), std::invalid_argument);
  EXPECT_EQ(clock.now_utc(), 500);  // unchanged after the refused call
}

}  // namespace
