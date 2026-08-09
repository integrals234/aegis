#include <gtest/gtest.h>

#include "cpp/exchange/sequencer/sequencer.hpp"

namespace {

using aegis::common::EventTime;
using aegis::events::CommandSequence;
using aegis::exchange::Sequencer;

TEST(Sequencer, StartsAtOneAndHasNoGaps) {
  Sequencer sequencer;
  EXPECT_EQ(sequencer.sequence(EventTime{0}), CommandSequence{1});
  EXPECT_EQ(sequencer.sequence(EventTime{0}), CommandSequence{2});
  EXPECT_EQ(sequencer.sequence(EventTime{0}), CommandSequence{3});
}

TEST(Sequencer, AssignsASequenceEvenToWhatWillBeRejected) {
  // The sequencer knows nothing about validation; every command it sees
  // consumes a CommandSequence, rejected or not (ADR-0012).
  Sequencer sequencer;
  const auto first = sequencer.sequence(EventTime{100});
  const auto second = sequencer.sequence(EventTime{200});
  EXPECT_NE(first, second);
}

TEST(Sequencer, ExchangeTimeIsMaxOfPreviousAndEventTime) {
  Sequencer sequencer;
  sequencer.sequence(EventTime{100});
  EXPECT_EQ(sequencer.last_exchange_time().nanos(), 100);

  sequencer.sequence(EventTime{500});
  EXPECT_EQ(sequencer.last_exchange_time().nanos(), 500);
}

TEST(Sequencer, RegressingEventTimeIsStampedForwardAndCounted) {
  Sequencer sequencer;
  sequencer.sequence(EventTime{500});
  ASSERT_EQ(sequencer.last_exchange_time().nanos(), 500);

  sequencer.sequence(EventTime{100});  // regresses relative to the prior command
  EXPECT_EQ(sequencer.last_exchange_time().nanos(), 500) << "must not regress";
  EXPECT_EQ(sequencer.regressing_event_time_count(), 1U);
}

TEST(Sequencer, TwoCommandsSharingAnEventTimeStillGetDistinctSequences) {
  Sequencer sequencer;
  const auto first = sequencer.sequence(EventTime{42});
  const auto second = sequencer.sequence(EventTime{42});
  EXPECT_LT(first, second);
}

TEST(Sequencer, RestoresPositionFromASnapshot) {
  const Sequencer restored{CommandSequence{100}, aegis::common::ExchangeTime{5000}};
  EXPECT_EQ(restored.next_command_sequence(), CommandSequence{100});
  EXPECT_EQ(restored.last_exchange_time().nanos(), 5000);
  EXPECT_EQ(restored.regressing_event_time_count(), 0U);
}

}  // namespace
