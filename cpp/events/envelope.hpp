#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/common/time.hpp"

/// Versioned message envelope and canonical encoding (ADR-0002).
///
/// Every runtime boundary in AEGIS carries versioned messages
/// (docs/ARCHITECTURE.md). This is the header they all share; the payload stays
/// opaque, because M0 has no domain message types and inventing them here would
/// be building M1 early.
///
/// The encoding is *canonical*: one value has exactly one byte representation.
/// That is what makes a replay hash meaningful — AEGIS-005 asks for byte-identical
/// canonical output from identical input, and an encoder with any freedom of
/// expression turns "the outputs differ" into a question about the encoder
/// rather than about the engine. Concretely:
///
/// * fixed field order, no optional fields, no padding;
/// * fixed-width little-endian integers, never native byte order;
/// * **no floating point anywhere** — the same double prints differently under
///   different libcs and rounds differently under different flags;
/// * length-prefixed strings, so no delimiter can be ambiguous;
/// * no map or set iteration order in the encoded form;
/// * no locale-dependent formatting.
///
/// `python/common/envelope.py` encodes the same bytes, and a shared golden
/// fixture holds both implementations to it.
namespace aegis::events {

inline constexpr std::uint16_t kEnvelopeSchemaVersion = 1;

/// Message types. Numbers are permanent: a retired type keeps its number
/// forever, because a recorded stream outlives the code that wrote it, and
/// reusing a number would make old recordings decode into the wrong type.
///
/// M0 defines exactly one: the platform envelope used for framing and
/// determinism fixtures. Domain types (orders, trades, book updates) arrive with
/// the exchange in M1.
/// The base type is part of the wire format: two bytes on the wire regardless of
/// how few values are currently defined, so adding the thousandth message type
/// does not reframe every recorded stream.
/// NOLINTNEXTLINE(performance-enum-size)
enum class MessageType : std::uint16_t {
  kUnspecified = 0,  ///< Platform framing; payload is opaque to the transport.

  // 1..999 reserved for exchange-side domain messages (M1, ADR-0009).
  // Commands: assigned no CommandSequence on the wire — the sequencer assigns
  // one on receipt. Framed with Envelope.sequence = CommandSequence.
  kNewOrder = 1,
  kCancelOrder = 2,
  kModifyOrder = 3,
  // Events: framed with Envelope.sequence = EventSequence.
  kOrderAccepted = 10,
  kOrderRejected = 11,
  kOrderModified = 12,
  kOrderReplaced = 13,
  kTrade = 14,
  kOrderTerminated = 15,

  // Market data (M3, AEGIS-064/065, ADR-0020): exchange-*published*, so these
  // take permanent numbers in this same exchange band rather than the
  // participant band below -- a participant decodes them without depending on
  // cpp-exchange-market-data, exactly as it decodes order/trade events without
  // depending on cpp-exchange-matching. See cpp/events/market_data_messages.hpp.
  kBookSnapshot = 20,
  kBookDelta = 21,

  // 1000..1999 reserved for participant-side domain messages (M3).
};

[[nodiscard]] bool is_known_message_type(std::uint16_t value);

/// Reasons a decode can fail. Returned rather than thrown: a malformed message
/// on a feed is an expected event that the feed handler must count and continue
/// past, not an exceptional one.
enum class DecodeError : std::uint8_t {
  kTruncated,
  kUnsupportedSchemaVersion,
  kUnknownMessageType,
  kLengthOverflow,
  kTrailingBytes,
};

[[nodiscard]] std::string_view describe(DecodeError error);

/// The header every AEGIS message carries.
struct Envelope {
  std::uint16_t schema_version{kEnvelopeSchemaVersion};
  MessageType message_type{MessageType::kUnspecified};

  /// Monotone within one stream. Gaps and repeats are detectable by the
  /// consumer, which is what AEGIS-068 sequence validation will rely on.
  std::uint64_t sequence{0};
  std::uint64_t stream_id{0};

  /// Source event time. An EventTime specifically: a monotonic reading cannot
  /// be placed here, because cpp/common/time.hpp refuses to serialize one.
  common::EventTime event_time;

  std::string producer_id;
  std::string experiment_id;   ///< The same field the structured logger carries.
  std::string correlation_id;  ///< Ties a message to one causal chain; may be empty.

  /// Opaque payload. The envelope neither parses nor interprets it.
  std::vector<std::byte> payload;

  friend bool operator==(const Envelope&, const Envelope&) = default;
};

/// The outcome of a decode: either an envelope or the reason there is none.
///
/// A hand-rolled result type rather than std::expected because AEGIS targets
/// C++20 (ADR-0005); switching the language standard is a decision for an ADR,
/// not a side effect of writing this file. The interface matches std::expected
/// closely enough that adopting it later is a mechanical change.
class DecodeResult {
 public:
  static DecodeResult success(Envelope envelope) {
    DecodeResult result;
    result.value_ = std::move(envelope);
    return result;
  }
  static DecodeResult failure(DecodeError error) {
    DecodeResult result;
    result.error_ = error;
    return result;
  }

  [[nodiscard]] bool has_value() const { return value_.has_value(); }
  /// Precondition: has_value(). Throws std::runtime_error otherwise, so a
  /// caller that forgets to check fails loudly rather than reading state that
  /// was never decoded.
  [[nodiscard]] const Envelope& value() const {
    if (!value_.has_value()) {
      throw std::runtime_error("DecodeResult::value() called on a failed decode: " +
                               std::string{describe(error_)});
    }
    return *value_;  // NOLINT(bugprone-unchecked-optional-access) - guarded above
  }
  [[nodiscard]] DecodeError error() const { return error_; }

 private:
  std::optional<Envelope> value_;
  DecodeError error_{DecodeError::kTruncated};
};

/// Encode an envelope into its canonical byte form.
[[nodiscard]] std::vector<std::byte> encode(const Envelope& envelope);

/// Decode a canonical byte form, or report why it could not be decoded.
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes);

/// Lowercase hexadecimal, for golden fixtures and failure messages.
[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> from_hex(std::string_view hex);

}  // namespace aegis::events
