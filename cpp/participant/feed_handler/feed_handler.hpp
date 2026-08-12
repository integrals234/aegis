#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "cpp/events/envelope.hpp"
#include "cpp/events/exchange_messages.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/participant/feed_handler/sequence_tracker.hpp"

/// Decodes the wire messages a participant receives and validates market-data
/// sequencing (AEGIS-068; ADR-0021). `cpp-participant-feed-handler` may
/// depend on `cpp-common` and `cpp-events` only
/// (`configs/architecture_rules.yaml`) — no `cpp-replay` edge, so fault
/// scenarios reach this class only as an ordinary (possibly faulted) message
/// stream the composition root feeds it, never through a direct dependency.
namespace aegis::participant::feed {

/// NOLINTNEXTLINE(performance-enum-size)
enum class DecodedKind : std::uint8_t {
  kBookSnapshot = 0,
  kBookDelta = 1,
  kTrade = 2,
  kOrderTerminated = 3,
  /// The envelope decoded, but its `message_type` is not one this handler
  /// interprets, or its payload failed to decode as that type.
  kUnhandled = 4,
};

/// One decoded message, tagged by `kind`. Exactly one of the `optional`
/// members is engaged, matching `kind` — a discriminated union expressed
/// with `std::optional` rather than `std::variant` so an unrecognized
/// `message_type` (`kUnhandled`) needs no placeholder alternative.
struct DecodedMessage {
  DecodedKind kind{DecodedKind::kUnhandled};
  std::optional<events::market_data::BookSnapshotEvent> snapshot;
  std::optional<events::market_data::BookDeltaEvent> delta;
  std::optional<events::exchange::TradeEvent> trade;
  std::optional<events::exchange::OrderTerminatedEvent> terminated;
  /// Engaged only for `kBookSnapshot`/`kBookDelta` — the sequence
  /// classification for that instrument's `md_sequence` stream at this
  /// message (AEGIS-068).
  std::optional<SequenceCheckResult> sequence_check;
};

class FeedHandler {
 public:
  FeedHandler() = default;

  /// Decodes `envelope.payload` according to `envelope.message_type` and, for
  /// a market-data message, runs its `md_sequence` through the sequence
  /// tracker for that message's `instrument_id` (a separate tracker per
  /// instrument, since sequence numbering is a per-instrument stream).
  [[nodiscard]] DecodedMessage decode(const events::Envelope& envelope);

  [[nodiscard]] const SequenceTracker* tracker_for(std::uint32_t instrument_id) const;

 private:
  std::unordered_map<std::uint32_t, SequenceTracker> trackers_;
};

}  // namespace aegis::participant::feed
