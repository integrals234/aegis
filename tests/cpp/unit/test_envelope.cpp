#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cpp/common/config.hpp"
#include "cpp/common/time.hpp"
#include "cpp/events/envelope.hpp"

/// ADR-0002, C++ side.
///
/// The golden fixture asserted here is the same file
/// tests/unit/test_envelope_golden.py asserts. Holding both encoders to one
/// committed byte string is what stops them drifting: without it, a change made
/// to one and not the other surfaces as a corrupt recording months later, by
/// which time the recordings that matter were written in the old format.
namespace {

using aegis::events::DecodeError;
using aegis::events::Envelope;
using aegis::events::MessageType;

nlohmann::json golden() {
  const std::filesystem::path root{AEGIS_SOURCE_ROOT};
  return aegis::common::load_schema(root / "tests/unit/fixtures/envelope/golden.json");
}

Envelope build(const nlohmann::json& fields) {
  Envelope envelope;
  envelope.sequence = fields.at("sequence").get<std::uint64_t>();
  envelope.stream_id = fields.at("stream_id").get<std::uint64_t>();
  envelope.event_time = aegis::common::EventTime{fields.at("event_time_ns").get<std::int64_t>()};
  envelope.producer_id = fields.at("producer_id").get<std::string>();
  envelope.experiment_id = fields.at("experiment_id").get<std::string>();
  envelope.correlation_id = fields.at("correlation_id").get<std::string>();
  envelope.payload = aegis::events::from_hex(fields.at("payload_hex").get<std::string>());
  return envelope;
}

TEST(Envelope, EncoderMatchesTheGoldenBytes) {
  const auto document = golden();
  ASSERT_FALSE(document.at("cases").empty()) << "an empty fixture asserts nothing";

  for (const auto& entry : document.at("cases")) {
    const auto envelope = build(entry.at("fields"));
    const auto encoded = aegis::events::encode(envelope);
    EXPECT_EQ(aegis::events::to_hex(encoded), entry.at("encoded_hex").get<std::string>())
        << entry.at("name").get<std::string>() << ": " << entry.at("why").get<std::string>();
  }
}

TEST(Envelope, GoldenBytesDecodeBackToTheCase) {
  for (const auto& entry : golden().at("cases")) {
    const auto bytes = aegis::events::from_hex(entry.at("encoded_hex").get<std::string>());
    const auto decoded = aegis::events::decode(bytes);
    ASSERT_TRUE(decoded.has_value()) << entry.at("name").get<std::string>();
    EXPECT_EQ(decoded.value(), build(entry.at("fields")));
  }
}

TEST(Envelope, RoundTripsEveryField) {
  Envelope envelope;
  envelope.sequence = 1234;
  envelope.stream_id = 77;
  envelope.event_time = aegis::common::EventTime{-1};  // pre-epoch stays exact
  envelope.producer_id = "exchange.sequencer";
  envelope.experiment_id = "m0-envelope";
  envelope.correlation_id = "order-4711";
  envelope.payload = aegis::events::from_hex("cafebabe");

  const auto decoded = aegis::events::decode(aegis::events::encode(envelope));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value(), envelope);
}

TEST(Envelope, TruncationIsDetected) {
  const auto encoded = aegis::events::encode(Envelope{});
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    const std::span<const std::byte> prefix{encoded.data(), length};
    EXPECT_FALSE(aegis::events::decode(prefix).has_value())
        << "a prefix of a valid message decoded as a message, at length " << length;
  }
}

TEST(Envelope, TrailingBytesAreRejected) {
  // A desynchronised stream must stop rather than keep decoding plausible
  // messages out of whatever follows.
  auto encoded = aegis::events::encode(Envelope{});
  encoded.push_back(std::byte{0x00});

  const auto decoded = aegis::events::decode(encoded);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), DecodeError::kTrailingBytes);
}

TEST(Envelope, UnknownSchemaVersionIsRefused) {
  auto encoded = aegis::events::encode(Envelope{});
  encoded[0] = std::byte{0x09};

  const auto decoded = aegis::events::decode(encoded);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), DecodeError::kUnsupportedSchemaVersion)
      << "guessing at an unknown layout yields plausible messages that are wrong";
}

TEST(Envelope, UnknownMessageTypeIsRefused) {
  auto encoded = aegis::events::encode(Envelope{});
  encoded[2] = std::byte{0x7F};

  const auto decoded = aegis::events::decode(encoded);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), DecodeError::kUnknownMessageType);
}

TEST(Envelope, DeclaredPayloadLongerThanTheBufferIsRefused) {
  Envelope envelope;
  envelope.payload = aegis::events::from_hex("0102030405");
  auto encoded = aegis::events::encode(envelope);
  encoded.resize(encoded.size() - 2);  // keep the length prefix, drop payload bytes

  const auto decoded = aegis::events::decode(encoded);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error(), DecodeError::kLengthOverflow);
}

TEST(Envelope, OversizedStringIsRejectedRatherThanTruncated) {
  // A 16-bit length prefix cannot describe a longer string; truncating would
  // corrupt this frame and every message after it.
  Envelope envelope;
  envelope.producer_id = std::string(70000, 'x');
  EXPECT_THROW(std::ignore = aegis::events::encode(envelope), std::length_error);
}

TEST(Envelope, EncodingIsAFunction) {
  Envelope envelope;
  envelope.sequence = 9;
  envelope.producer_id = "p";
  EXPECT_EQ(aegis::events::encode(envelope), aegis::events::encode(envelope))
      << "a replay hash would otherwise measure the encoder rather than the engine";
}

TEST(Envelope, EveryDecodeErrorHasAHumanReadableReason) {
  for (const auto error : {DecodeError::kTruncated, DecodeError::kUnsupportedSchemaVersion,
                           DecodeError::kUnknownMessageType, DecodeError::kLengthOverflow,
                           DecodeError::kTrailingBytes}) {
    EXPECT_FALSE(aegis::events::describe(error).empty());
  }
}

TEST(Envelope, OnlyTheUnspecifiedMessageTypeExistsAtM0) {
  // Domain message types belong to M1 and M3. Defining them here would be
  // building the exchange early, and their numbers are permanent once written.
  EXPECT_TRUE(
      aegis::events::is_known_message_type(static_cast<std::uint16_t>(MessageType::kUnspecified)));
  EXPECT_FALSE(aegis::events::is_known_message_type(1));
  EXPECT_FALSE(aegis::events::is_known_message_type(1000));
}

}  // namespace
