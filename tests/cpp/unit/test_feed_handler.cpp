#include <cstddef>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/participant/feed_handler/feed_handler.hpp"
#include "tests/cpp/optional_access.hpp"

namespace {

using aegis::events::Envelope;
using aegis::events::MessageType;
using aegis::events::exchange::Side;
using aegis::events::exchange::TradeEvent;
using aegis::events::market_data::BookLevelEntry;
using aegis::events::market_data::BookSnapshotEvent;
using aegis::participant::feed::DecodedKind;
using aegis::participant::feed::FeedHandler;
using aegis::participant::feed::SequenceDiagnostic;

Envelope frame(MessageType type, std::vector<std::byte> payload) {
  Envelope envelope;
  envelope.message_type = type;
  envelope.payload = std::move(payload);
  return envelope;
}

TEST(FeedHandler, DecodesABookSnapshotAndTracksItsSequence) {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = 7;
  snapshot.md_sequence = 1;
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 100, .quantity_units = 10, .order_id = 1});

  FeedHandler handler;
  const auto decoded = handler.decode(frame(MessageType::kBookSnapshot, encode(snapshot)));

  ASSERT_EQ(decoded.kind, DecodedKind::kBookSnapshot);
  ASSERT_TRUE(decoded.snapshot.has_value());
  EXPECT_EQ(aegis::test::checked(decoded.snapshot).instrument_id, 7U);
  ASSERT_TRUE(decoded.sequence_check.has_value());
  EXPECT_EQ(aegis::test::checked(decoded.sequence_check).diagnostic, SequenceDiagnostic::kOk);
}

TEST(FeedHandler, TracksSequenceIndependentlyPerInstrument) {
  FeedHandler handler;
  BookSnapshotEvent first;
  first.instrument_id = 1;
  first.md_sequence = 5;
  BookSnapshotEvent second;
  second.instrument_id = 2;
  second.md_sequence = 1;

  const auto first_decoded = handler.decode(frame(MessageType::kBookSnapshot, encode(first)));
  const auto second_decoded = handler.decode(frame(MessageType::kBookSnapshot, encode(second)));

  EXPECT_EQ(aegis::test::checked(first_decoded.sequence_check).diagnostic, SequenceDiagnostic::kOk);
  EXPECT_EQ(aegis::test::checked(second_decoded.sequence_check).diagnostic,
            SequenceDiagnostic::kOk);
  ASSERT_NE(handler.tracker_for(1), nullptr);
  ASSERT_NE(handler.tracker_for(2), nullptr);
  EXPECT_EQ(handler.tracker_for(1)->last_sequence(), 5U);
  EXPECT_EQ(handler.tracker_for(2)->last_sequence(), 1U);
}

TEST(FeedHandler, DecodesATrade) {
  TradeEvent trade;
  trade.instrument_id = 1;
  trade.price_units = 100;
  trade.quantity_units = 10;

  FeedHandler handler;
  const auto decoded = handler.decode(frame(MessageType::kTrade, encode(trade)));
  ASSERT_EQ(decoded.kind, DecodedKind::kTrade);
  ASSERT_TRUE(decoded.trade.has_value());
  EXPECT_EQ(aegis::test::checked(decoded.trade).price_units, 100);
  EXPECT_FALSE(decoded.sequence_check.has_value());  // Trades carry no md_sequence.
}

TEST(FeedHandler, UnrecognizedMessageTypeIsUnhandled) {
  FeedHandler handler;
  const auto decoded = handler.decode(frame(MessageType::kUnspecified, {}));
  EXPECT_EQ(decoded.kind, DecodedKind::kUnhandled);
}

TEST(FeedHandler, MalformedPayloadIsUnhandled) {
  FeedHandler handler;
  const auto decoded = handler.decode(frame(MessageType::kBookSnapshot, {std::byte{0x01}}));
  EXPECT_EQ(decoded.kind, DecodedKind::kUnhandled);
}

}  // namespace
