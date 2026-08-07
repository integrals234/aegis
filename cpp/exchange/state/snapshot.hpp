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
#include "cpp/exchange/order_book/book.hpp"
#include "cpp/exchange/sequencer/sequencer.hpp"
#include "cpp/exchange/state/event_log.hpp"

/// Exchange snapshot codec (ADR-0013): the on-wire form of everything a
/// restored `ExchangeNode` needs to resume command processing with the same
/// runtime position an uninterrupted run would have — the three counters
/// (`CommandSequence`, `EventSequence`, `OrderId`, ADR-0012) plus every
/// resting order across every registered instrument.
///
/// `cpp-exchange-state` may depend on `cpp-common`, `cpp-events`,
/// `cpp-exchange-sequencer` and `cpp-exchange-order-book` only — never on
/// `cpp-exchange-app` (`configs/architecture_rules.yaml`) — so this header
/// knows nothing about `ExchangeNode`. Turning an `ExchangeSnapshot` into a
/// running exchange is the composition root's job.
namespace aegis::exchange {

inline constexpr std::uint32_t kSnapshotVersion = 1;

/// One resting order as captured/restored. Distinct from `OrderNode`
/// (`cpp/exchange/order_book/order_storage.hpp`) because that struct also
/// carries the intrusive FIFO queue handles (`prev_index`/`next_index`),
/// which are slab-local and meaningless outside the process that produced
/// them.
struct SnapshotOrderRecord {
  std::uint64_t order_id{0};
  std::uint32_t instrument_id{0};
  std::uint64_t participant_id{0};
  std::uint64_t client_order_id{0};
  Side side{Side::kBuy};
  OrderType order_type{OrderType::kLimit};
  std::int64_t price_units{0};
  std::int64_t original_quantity{0};
  std::int64_t cumulative_filled{0};
  std::int64_t cancelled_quantity{0};
  std::int64_t remaining{0};
  std::uint64_t priority_command_sequence{0};

  friend bool operator==(const SnapshotOrderRecord&, const SnapshotOrderRecord&) = default;
};

struct ExchangeSnapshot {
  std::uint32_t snapshot_version{kSnapshotVersion};
  std::uint64_t next_command_sequence{1};
  std::uint64_t next_event_sequence{1};
  std::uint64_t next_order_id{1};
  std::int64_t last_exchange_time_nanos{0};
  /// Canonical order: `(instrument_id, price_units, priority_command_sequence)`,
  /// all ascending (ADR-0013). `restore_orders_into` depends on this order to
  /// reconstruct each level's FIFO queue correctly, since `OrderBook::add`
  /// always appends to the tail regardless of the descriptor's own priority.
  std::vector<SnapshotOrderRecord> orders;

  friend bool operator==(const ExchangeSnapshot&, const ExchangeSnapshot&) = default;
};

/// Gathers every resting order across `books` into one `ExchangeSnapshot`, in
/// canonical order. `next_order_id` is not derivable from `books` alone (a
/// rejected `NewOrder` and a cancel-replace's terminated original both
/// consume no slab slot but do consume an `OrderId`), so the caller passes
/// `MatchingEngine::next_order_id()` explicitly.
[[nodiscard]] ExchangeSnapshot capture_snapshot(const Sequencer& sequencer,
                                                const EventLog& event_log,
                                                std::uint64_t next_order_id,
                                                std::span<const OrderBook* const> books);

/// Byte-stable canonical encoding (`cpp/events/wire.hpp` primitives): fixed
/// field order, fixed-width little-endian, no floating point.
[[nodiscard]] std::vector<std::byte> write_snapshot(const ExchangeSnapshot& snapshot);

/// Reasons `read_snapshot` refuses a byte sequence (ADR-0013).
enum class SnapshotError : std::uint8_t {
  kTruncated,
  /// `snapshot_version` is not `kSnapshotVersion`.
  kUnknownVersion,
  /// A header counter does not dominate its own contents: `next_order_id` at
  /// or below some restored order's `order_id`, or `next_command_sequence`
  /// at or below some restored order's `priority_command_sequence`. Either
  /// would hand out an identifier that collides with one already live in the
  /// restored book.
  kCounterInconsistent,
};

[[nodiscard]] std::string_view describe(SnapshotError error);

/// The outcome of `read_snapshot`: either a validated snapshot or the reason
/// there is none. Mirrors `events::DecodeResult`/`DecodeError`
/// (`cpp/events/envelope.hpp`).
class SnapshotReadResult {
 public:
  [[nodiscard]] static SnapshotReadResult success(ExchangeSnapshot snapshot);
  [[nodiscard]] static SnapshotReadResult failure(SnapshotError error);

  [[nodiscard]] bool has_value() const { return value_.has_value(); }
  /// Precondition: has_value(). Throws std::runtime_error otherwise, so a
  /// caller that forgets to check fails loudly rather than reading state
  /// that was never restored. `.value_or(ExchangeSnapshot{})` is the checked
  /// alternative for a caller that has not already asserted `has_value()`.
  [[nodiscard]] const ExchangeSnapshot& value() const {
    if (!value_.has_value()) {
      throw std::runtime_error("SnapshotReadResult::value() called on a failed read: " +
                               std::string{describe(error_)});
    }
    return *value_;  // NOLINT(bugprone-unchecked-optional-access) - guarded above
  }
  [[nodiscard]] ExchangeSnapshot value_or(const ExchangeSnapshot& fallback) const {
    return value_.has_value() ? *value_ : fallback;
  }
  [[nodiscard]] SnapshotError error() const { return error_; }

 private:
  std::optional<ExchangeSnapshot> value_;
  SnapshotError error_{SnapshotError::kTruncated};
};

[[nodiscard]] SnapshotReadResult read_snapshot(std::span<const std::byte> bytes);

/// Replays every record in `snapshot` whose `instrument_id` matches `book`'s
/// into `book`, via `OrderBook::add`, in the snapshot's stored (canonical)
/// order — the order that reconstructs each level's FIFO queue exactly
/// (ADR-0013). The caller drives one call per registered instrument, since
/// this layer may not depend on `cpp-exchange-app` to discover them itself.
void restore_orders_into(OrderBook& book, InstrumentId instrument_id,
                         const ExchangeSnapshot& snapshot);

}  // namespace aegis::exchange
