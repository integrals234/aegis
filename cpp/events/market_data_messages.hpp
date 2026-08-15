#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "cpp/events/exchange_messages.hpp"

/// Market-data wire vocabulary (AEGIS-064, AEGIS-065; ADR-0020).
///
/// Lives in `cpp/events`, alongside the exchange domain vocabulary it reuses
/// `Side` from, for the same reason ADR-0009 gives for that vocabulary: the M3
/// participant decodes what the exchange publishes without depending on the
/// exchange layer at all. `cpp/exchange/market_data` is the one *producer* of
/// these messages; `cpp/participant/feed_handler` and `cpp/participant/book_builder`
/// are consumers, and neither depends on the other's layer to share this type.
///
/// Two message kinds, matching the two things a book-reconstruction consumer
/// needs (AEGIS-064/065/066/067):
///
/// * `BookSnapshotEvent` — the complete state of one instrument's book at one
///   moment, as a flat list of per-order entries in canonical FIFO order
///   (oldest first) within each price. A consumer that only wants aggregated
///   depth sums entries at equal price; a consumer that wants order-level
///   detail (AEGIS-066) already has it, since every entry names its order.
/// * `BookDeltaEvent` — one incremental change since the previous `md_sequence`.
///   `order_id` nonzero names an order-level change (AEGIS-066: added,
///   modified in place, or removed); `order_id == 0` names a pure aggregated
///   price-level change (AEGIS-067) for a source that never exposes order
///   identity. A feed commits to one or the other for its whole session — a
///   consumer does not need to guess which on a message-by-message basis.
namespace aegis::events::market_data {

/// One order's contribution to a `BookSnapshotEvent`, or one order-level
/// change in a `BookDeltaEvent`. `order_id == 0` is reserved for
/// `BookDeltaEvent`'s price-level-only form and never appears in a snapshot
/// entry, since a snapshot always observes the book's real orders.
struct BookLevelEntry {
  exchange::Side side{exchange::Side::kBuy};
  std::int64_t price_units{0};
  std::int64_t quantity_units{0};  ///< This order's remaining quantity.
  std::uint64_t order_id{0};

  friend bool operator==(const BookLevelEntry&, const BookLevelEntry&) = default;
};

/// A complete, self-sufficient book state (AEGIS-064). `md_sequence` is the
/// market-data sequence space -- a separate counter from `EventSequence`
/// (`cpp/events/sequence.hpp`), since a snapshot is not itself caused by one
/// exchange command. A consumer applying this snapshot discards any prior
/// state and any buffered delta at or below `md_sequence` (AEGIS-070).
struct BookSnapshotEvent {
  std::uint32_t instrument_id{0};
  std::uint64_t md_sequence{0};
  /// Canonical order: bids by descending price then FIFO arrival, asks by
  /// ascending price then FIFO arrival -- the same order `orders_at` returns
  /// on the exchange side, so a snapshot round-trips through this type
  /// without reordering.
  std::vector<BookLevelEntry> entries;

  friend bool operator==(const BookSnapshotEvent&, const BookSnapshotEvent&) = default;
};

/// NOLINTNEXTLINE(performance-enum-size)
enum class DeltaKind : std::uint8_t {
  kOrderAdded = 0,
  kOrderModified = 1,
  kOrderRemoved = 2,
  /// Sets the aggregate quantity at `price_units` to `quantity_units`
  /// outright (0 means the level no longer exists) -- the aggregated-only
  /// form for a feed with no order identity (AEGIS-067). `order_id` is 0.
  kPriceLevelSet = 3,
};

[[nodiscard]] bool is_known_delta_kind(std::uint8_t value);

/// One incremental change, ordered by `md_sequence` within one instrument
/// (AEGIS-065, AEGIS-068). `quantity_units` means the order's new remaining
/// quantity for `kOrderAdded`/`kOrderModified`, is unused (encoded as 0) for
/// `kOrderRemoved`, and means the level's new aggregate quantity for
/// `kPriceLevelSet`.
struct BookDeltaEvent {
  std::uint32_t instrument_id{0};
  std::uint64_t md_sequence{0};
  DeltaKind kind{DeltaKind::kOrderAdded};
  std::uint64_t order_id{0};
  exchange::Side side{exchange::Side::kBuy};
  std::int64_t price_units{0};
  std::int64_t quantity_units{0};

  friend bool operator==(const BookDeltaEvent&, const BookDeltaEvent&) = default;
};

[[nodiscard]] std::vector<std::byte> encode(const BookSnapshotEvent& event);
[[nodiscard]] std::optional<BookSnapshotEvent> decode_book_snapshot(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode(const BookDeltaEvent& event);
[[nodiscard]] std::optional<BookDeltaEvent> decode_book_delta(std::span<const std::byte> bytes);

}  // namespace aegis::events::market_data
