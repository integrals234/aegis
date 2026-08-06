#include "cpp/events/envelope.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aegis::events {
namespace {

constexpr std::size_t kMaxStringLength = std::numeric_limits<std::uint16_t>::max();
constexpr std::string_view kHexDigits = "0123456789abcdef";

/// Append an unsigned integer little-endian.
///
/// Explicit little-endian rather than a memcpy of the native representation: a
/// recorded stream must decode identically on any machine, and "it works here"
/// is not a property of a file format.
template <typename T>
void put_unsigned(std::vector<std::byte>& out, T value) {
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    out.push_back(static_cast<std::byte>((value >> (8U * index)) & 0xFFU));
  }
}

template <typename T>
T get_unsigned(std::span<const std::byte> bytes, std::size_t offset) {
  T value{0};
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    value |= static_cast<T>(std::to_integer<std::uint64_t>(bytes[offset + index]) << (8U * index));
  }
  return value;
}

void put_string(std::vector<std::byte>& out, std::string_view text) {
  if (text.size() > kMaxStringLength) {
    // Throwing, unlike a decode failure: an oversized identifier is a caller
    // bug, not a data event. Truncating to fit the 16-bit prefix would corrupt
    // the frame and every message that follows it in the stream.
    throw std::length_error("envelope string field is " + std::to_string(text.size()) +
                            " bytes, over the " + std::to_string(kMaxStringLength) +
                            "-byte limit imposed by its 16-bit length prefix");
  }
  put_unsigned<std::uint16_t>(out, static_cast<std::uint16_t>(text.size()));
  for (const char character : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
}

/// Reads a length-prefixed string, advancing `offset`. Returns false when the
/// buffer is too short — the caller turns that into DecodeError::kTruncated.
bool take_string(std::span<const std::byte> bytes, std::size_t& offset, std::string& out) {
  if (offset + sizeof(std::uint16_t) > bytes.size()) {
    return false;
  }
  const auto length = get_unsigned<std::uint16_t>(bytes, offset);
  offset += sizeof(std::uint16_t);
  if (offset + length > bytes.size()) {
    return false;
  }
  out.assign(length, '\0');
  for (std::size_t index = 0; index < length; ++index) {
    out.at(index) = static_cast<char>(std::to_integer<unsigned char>(bytes[offset + index]));
  }
  offset += length;
  return true;
}

}  // namespace

bool is_known_message_type(std::uint16_t value) {
  return value == static_cast<std::uint16_t>(MessageType::kUnspecified);
}

std::string_view describe(DecodeError error) {
  switch (error) {
    case DecodeError::kTruncated:
      return "message ends before its declared fields do";
    case DecodeError::kUnsupportedSchemaVersion:
      return "envelope schema version is not understood by this build";
    case DecodeError::kUnknownMessageType:
      return "message type is not defined in this build";
    case DecodeError::kLengthOverflow:
      return "declared payload length exceeds the bytes supplied";
    case DecodeError::kTrailingBytes:
      return "message carries bytes beyond its declared payload";
  }
  return "unknown decode error";
}

std::vector<std::byte> encode(const Envelope& envelope) {
  std::vector<std::byte> out;
  out.reserve(64 + envelope.payload.size());

  // Field order is fixed and total. There are no optional fields: an encoder
  // with choices produces two byte strings for one value, and a replay hash
  // then measures the encoder rather than the engine.
  put_unsigned<std::uint16_t>(out, envelope.schema_version);
  put_unsigned<std::uint16_t>(out, static_cast<std::uint16_t>(envelope.message_type));
  put_unsigned<std::uint64_t>(out, envelope.sequence);
  put_unsigned<std::uint64_t>(out, envelope.stream_id);

  // Timestamps go through serialize_nanos, which refuses a MonotonicTime at
  // compile time. Signed nanoseconds are reinterpreted as unsigned for encoding
  // so the byte pattern is defined for negative values too.
  const auto event_nanos = common::serialize_nanos(envelope.event_time);
  put_unsigned<std::uint64_t>(out, static_cast<std::uint64_t>(event_nanos));

  put_string(out, envelope.producer_id);
  put_string(out, envelope.experiment_id);
  put_string(out, envelope.correlation_id);

  put_unsigned<std::uint32_t>(out, static_cast<std::uint32_t>(envelope.payload.size()));
  out.insert(out.end(), envelope.payload.begin(), envelope.payload.end());
  return out;
}

DecodeResult decode(std::span<const std::byte> bytes) {
  constexpr std::size_t kFixedPrefix = 2 + 2 + 8 + 8 + 8;
  if (bytes.size() < kFixedPrefix) {
    return DecodeResult::failure(DecodeError::kTruncated);
  }

  Envelope envelope;
  std::size_t offset = 0;

  envelope.schema_version = get_unsigned<std::uint16_t>(bytes, offset);
  offset += sizeof(std::uint16_t);
  if (envelope.schema_version != kEnvelopeSchemaVersion) {
    // Refused, not tolerated: a build that guesses at an unknown layout produces
    // plausible messages that are wrong, and the run that follows looks normal.
    return DecodeResult::failure(DecodeError::kUnsupportedSchemaVersion);
  }

  const auto raw_type = get_unsigned<std::uint16_t>(bytes, offset);
  offset += sizeof(std::uint16_t);
  if (!is_known_message_type(raw_type)) {
    return DecodeResult::failure(DecodeError::kUnknownMessageType);
  }
  envelope.message_type = static_cast<MessageType>(raw_type);

  envelope.sequence = get_unsigned<std::uint64_t>(bytes, offset);
  offset += sizeof(std::uint64_t);
  envelope.stream_id = get_unsigned<std::uint64_t>(bytes, offset);
  offset += sizeof(std::uint64_t);
  envelope.event_time =
      common::EventTime{static_cast<common::Nanos>(get_unsigned<std::uint64_t>(bytes, offset))};
  offset += sizeof(std::uint64_t);

  if (!take_string(bytes, offset, envelope.producer_id) ||
      !take_string(bytes, offset, envelope.experiment_id) ||
      !take_string(bytes, offset, envelope.correlation_id)) {
    return DecodeResult::failure(DecodeError::kTruncated);
  }

  if (offset + sizeof(std::uint32_t) > bytes.size()) {
    return DecodeResult::failure(DecodeError::kTruncated);
  }
  const auto payload_length = get_unsigned<std::uint32_t>(bytes, offset);
  offset += sizeof(std::uint32_t);
  if (offset + payload_length > bytes.size()) {
    return DecodeResult::failure(DecodeError::kLengthOverflow);
  }
  envelope.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_length));
  offset += payload_length;

  if (offset != bytes.size()) {
    // Trailing bytes mean the framing is wrong somewhere upstream. Accepting
    // them silently would let a desynchronised stream keep decoding.
    return DecodeResult::failure(DecodeError::kTrailingBytes);
  }
  return DecodeResult::success(std::move(envelope));
}

std::string to_hex(std::span<const std::byte> bytes) {
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned char>(byte);
    hex.push_back(kHexDigits.at(static_cast<std::size_t>(value) >> 4U));
    hex.push_back(kHexDigits.at(static_cast<std::size_t>(value) & 0x0FU));
  }
  return hex;
}

std::vector<std::byte> from_hex(std::string_view hex) {
  std::vector<std::byte> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    const auto high = kHexDigits.find(hex.at(index));
    const auto low = kHexDigits.find(hex.at(index + 1));
    if (high == std::string_view::npos || low == std::string_view::npos) {
      return {};
    }
    bytes.push_back(static_cast<std::byte>((high << 4U) | low));
  }
  return bytes;
}

}  // namespace aegis::events
