#include <gtest/gtest.h>

#include "cpp/events/market_data_messages.hpp"

namespace {

using aegis::events::exchange::Side;
using aegis::events::market_data::BookDeltaEvent;
using aegis::events::market_data::BookLevelEntry;
using aegis::events::market_data::BookSnapshotEvent;
using aegis::events::market_data::decode_book_delta;
using aegis::events::market_data::decode_book_snapshot;
using aegis::events::market_data::DeltaKind;

TEST(MarketDataMessages, SnapshotRoundTripsThroughEncodeDecode) {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = 42;
  snapshot.md_sequence = 7;
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 100, .quantity_units = 10, .order_id = 1});
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kSell, .price_units = 105, .quantity_units = 5, .order_id = 2});

  const auto decoded = decode_book_snapshot(encode(snapshot));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value(), snapshot);
}

TEST(MarketDataMessages, EmptySnapshotRoundTrips) {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = 1;
  snapshot.md_sequence = 1;
  const auto decoded = decode_book_snapshot(encode(snapshot));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded.value().entries.empty());
}

TEST(MarketDataMessages, TruncatedSnapshotFailsToDecode) {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = 1;
  snapshot.md_sequence = 1;
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 1, .quantity_units = 1, .order_id = 1});
  auto bytes = encode(snapshot);
  bytes.resize(bytes.size() - 1);
  EXPECT_FALSE(decode_book_snapshot(bytes).has_value());
}

TEST(MarketDataMessages, DeltaRoundTripsThroughEncodeDecode) {
  BookDeltaEvent delta;
  delta.instrument_id = 42;
  delta.md_sequence = 3;
  delta.kind = DeltaKind::kOrderModified;
  delta.order_id = 9;
  delta.side = Side::kSell;
  delta.price_units = 200;
  delta.quantity_units = 15;

  const auto decoded = decode_book_delta(encode(delta));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value(), delta);
}

TEST(MarketDataMessages, TruncatedDeltaFailsToDecode) {
  BookDeltaEvent delta;
  delta.instrument_id = 1;
  auto bytes = encode(delta);
  bytes.resize(bytes.size() - 1);
  EXPECT_FALSE(decode_book_delta(bytes).has_value());
}

TEST(MarketDataMessages, UnknownDeltaKindFailsToDecode) {
  BookDeltaEvent delta;
  delta.instrument_id = 1;
  delta.kind = DeltaKind::kPriceLevelSet;
  auto bytes = encode(delta);
  // The kind byte is at index 12: 4 (instrument_id) + 8 (md_sequence).
  bytes[12] = static_cast<std::byte>(0xFF);
  EXPECT_FALSE(decode_book_delta(bytes).has_value());
}

}  // namespace
