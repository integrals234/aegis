#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "cpp/events/envelope.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/exchange/order_book/book.hpp"

/// The market-data publisher (AEGIS-064, AEGIS-065; ADR-0020).
///
/// Observes an `OrderBook` and the matching-emitted event stream
/// `cpp-exchange-app` already has (`MatchingEngine::apply_*`'s return value);
/// never calls into matching and never mutates a book. `cpp-exchange-market-data`
/// may not depend on `cpp-exchange-matching` (`configs/architecture_rules.yaml`),
/// so it does not name `MatchingEngine::EmittedEvent` — `EmittedMessage` below
/// is the identical shape, expressed only in terms of `cpp-events`, that the
/// composition root converts to before calling `observe()`. This layer is
/// exchange-*published*: the wire types it emits
/// (`cpp/events/market_data_messages.hpp`) are decodable by a participant that
/// has never heard of `cpp-exchange-market-data`, the same way an order/trade
/// event is decodable without depending on `cpp-exchange-matching` (ADR-0009).
namespace aegis::exchange {

/// One already-emitted matching event, in wire form. Shape-identical to
/// `MatchingEngine::EmittedEvent`; kept as a separate type because this layer
/// may not depend on `cpp-exchange-matching` to name that one directly.
struct EmittedMessage {
  events::MessageType message_type{events::MessageType::kUnspecified};
  std::vector<std::byte> payload;
};

/// A full-depth snapshot of `book`'s current state, at market-data sequence
/// `md_sequence`, in canonical FIFO order per level. Read-only over `book` —
/// this call has no effect on matching or book state.
[[nodiscard]] events::market_data::BookSnapshotEvent capture_book_snapshot(
    const OrderBook& book, std::uint64_t md_sequence);

/// Derives incremental book-depth changes from a batch of already-emitted
/// matching events (`MatchingEngine::apply_*`'s return value), assigning each
/// resulting delta the next `md_sequence` from an internal counter this
/// object owns. Order-level: every accepted, modified, replaced or
/// terminated order produces one delta naming its `order_id`, mirroring
/// `OrderBook`'s own state without querying it — this class tracks
/// `order_id -> (instrument, side, price)` purely from the events it has
/// already seen, since `OrderModifiedEvent`/`OrderTerminatedEvent` do not
/// themselves carry that context.
class MarketDataPublisher {
 public:
  MarketDataPublisher() = default;

  /// Processes `emitted` in order and returns the deltas it produces, if any.
  /// A `TradeEvent` or `OrderRejectedEvent` produces no delta directly — the
  /// resting-side depth change they cause already arrives via its own
  /// `OrderModified`/`OrderTerminated` event.
  [[nodiscard]] std::vector<events::market_data::BookDeltaEvent> observe(
      const std::vector<EmittedMessage>& emitted);

  [[nodiscard]] std::uint64_t next_md_sequence() const { return next_md_sequence_; }

 private:
  struct TrackedOrder {
    std::uint32_t instrument_id{0};
    Side side{Side::kBuy};
    std::int64_t price_units{0};
  };

  std::unordered_map<std::uint64_t, TrackedOrder> tracked_;
  std::uint64_t next_md_sequence_{1};
};

}  // namespace aegis::exchange
