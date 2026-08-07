#include <gtest/gtest.h>

#include "cpp/events/exchange_messages.hpp"
#include "cpp/exchange/state/event_log.hpp"

namespace {

using aegis::common::EventTime;
using aegis::events::EventSequence;
using aegis::events::MessageType;
using aegis::exchange::EventLog;

TEST(EventLog, StartsAtOneAndAdvancesOncePerAppendedEvent) {
  EventLog log;
  const auto first = log.append(MessageType::kOrderAccepted, {}, EventTime{0});
  const auto second = log.append(MessageType::kTrade, {}, EventTime{0});
  EXPECT_EQ(first, EventSequence{1});
  EXPECT_EQ(second, EventSequence{2});
  EXPECT_EQ(log.next_event_sequence(), EventSequence{3});
}

TEST(EventLog, OneCommandProducingSeveralEventsAdvancesTheCounterOncePerEvent) {
  // An accept plus two trades plus a termination is four events from one
  // command — the counter must reflect events emitted, not commands seen.
  EventLog log;
  for (int i = 0; i < 4; ++i) {
    log.append(MessageType::kOrderAccepted, {}, EventTime{0});
  }
  EXPECT_EQ(log.envelopes().size(), 4U);
  EXPECT_EQ(log.next_event_sequence(), EventSequence{5});
}

TEST(EventLog, AppendedEnvelopeCarriesTheAssignedSequenceAndPayload) {
  EventLog log;
  const auto payload =
      aegis::events::exchange::encode(aegis::events::exchange::OrderTerminatedEvent{
          .causing_command_sequence = 1,
          .order_id = 2,
          .reason = aegis::events::exchange::TerminationReason::kFilled,
          .cancelled_quantity_delta_units = 0});
  log.append(MessageType::kOrderTerminated, payload, EventTime{777});

  ASSERT_EQ(log.envelopes().size(), 1U);
  const auto& envelope = log.envelopes().front();
  EXPECT_EQ(envelope.sequence, 1U);
  EXPECT_EQ(envelope.message_type, MessageType::kOrderTerminated);
  EXPECT_EQ(envelope.event_time.nanos(), 777);
  EXPECT_EQ(envelope.payload, payload);
}

TEST(EventLog, CanonicalLinesAreOneHexStringPerEnvelopeInSequenceOrder) {
  EventLog log;
  log.append(MessageType::kOrderAccepted, {}, EventTime{1});
  log.append(MessageType::kTrade, {}, EventTime{2});

  const auto lines = log.to_canonical_lines();
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_EQ(lines[0], aegis::events::to_hex(aegis::events::encode(log.envelopes()[0])));
  EXPECT_EQ(lines[1], aegis::events::to_hex(aegis::events::encode(log.envelopes()[1])));
}

TEST(EventLog, RestoresPositionFromASnapshot) {
  const EventLog restored{EventSequence{500}};
  EXPECT_EQ(restored.next_event_sequence(), EventSequence{500});
  EXPECT_TRUE(restored.envelopes().empty());
}

}  // namespace
