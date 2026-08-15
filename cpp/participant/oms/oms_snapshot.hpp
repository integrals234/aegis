#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/participant/oms/order_manager.hpp"

/// OMS snapshot codec (AEGIS-237; ADR-0024): the on-wire form of every
/// `TrackedOrder` `OrderManager` holds, plus the counter needed to keep
/// assigning `client_order_id`s without collision after a restore.
///
/// `cpp-participant-oms` owns this codec because it owns the state inside
/// it (`docs/RECOVERY_CONTRACT.md` obligation 1) -- this header knows
/// nothing about portfolio state or about how the app layer combines this
/// snapshot with the portfolio's own. Mirrors
/// `cpp/exchange/state/snapshot.hpp` (ADR-0013) in shape and discipline:
/// byte-stable, version-refusing, round-trip verified.
namespace aegis::participant::oms {

inline constexpr std::uint32_t kOmsSnapshotVersion = 1;

/// One tracked order as captured/restored. Distinct from `TrackedOrder`
/// itself only in that `lifecycle_state` is the raw `OrderState` integer
/// rather than an `OrderLifecycle` object -- the wire has no notion of "the
/// legal-transition-table type," only the state it landed in.
struct OmsOrderRecord {
  std::uint64_t client_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint32_t instrument_id{0};
  std::uint64_t participant_id{0};
  events::exchange::Side side{events::exchange::Side::kBuy};
  std::uint8_t lifecycle_state{0};  ///< OrderState, as its underlying integer.
  std::int64_t price_units{0};
  std::int64_t original_quantity_units{0};
  std::int64_t cumulative_filled_units{0};
  std::int64_t remaining_units{0};
  /// AEGIS-116 realized cost state. Persisted because it is *accumulated*
  /// accounting, exactly like `cumulative_filled_units`: a restored order
  /// that forgot the fees it already paid would under-report net P&L for the
  /// rest of its life. The latency attribution is deliberately NOT persisted
  /// alongside it -- that is derived from a market event, not accumulated.
  std::int64_t cumulative_fees_units{0};
  std::int64_t cumulative_slippage_cost_units{0};

  friend bool operator==(const OmsOrderRecord&, const OmsOrderRecord&) = default;
};

struct OmsSnapshot {
  std::uint32_t snapshot_version{kOmsSnapshotVersion};
  std::uint64_t next_client_order_id{1};
  /// Canonical order: ascending `client_order_id` (matches
  /// `OrderManager::all_tracked_orders()`).
  std::vector<OmsOrderRecord> orders;

  friend bool operator==(const OmsSnapshot&, const OmsSnapshot&) = default;
};

/// Gathers every tracked order from `manager` into one `OmsSnapshot`, in
/// canonical (ascending `client_order_id`) order.
[[nodiscard]] OmsSnapshot capture_oms_snapshot(const OrderManager& manager);

/// Byte-stable canonical encoding (`cpp/events/wire.hpp` primitives): fixed
/// field order, fixed-width little-endian, no floating point.
[[nodiscard]] std::vector<std::byte> write_oms_snapshot(const OmsSnapshot& snapshot);

/// Reasons `read_oms_snapshot` refuses a byte sequence (mirrors
/// `exchange::SnapshotError`, ADR-0013/ADR-0024).
enum class OmsSnapshotError : std::uint8_t {
  kTruncated,
  /// `snapshot_version` is not `kOmsSnapshotVersion`.
  kUnknownVersion,
  /// A record's `side` or `lifecycle_state` is not a value this build knows,
  /// or `next_client_order_id` does not dominate every restored order's
  /// `client_order_id` -- either would hand out an id that collides with one
  /// already tracked, or resurrect a state this build cannot legally reach.
  kCounterInconsistent,
};

[[nodiscard]] std::string_view describe(OmsSnapshotError error);

class OmsSnapshotReadResult {
 public:
  [[nodiscard]] static OmsSnapshotReadResult success(OmsSnapshot snapshot);
  [[nodiscard]] static OmsSnapshotReadResult failure(OmsSnapshotError error);

  [[nodiscard]] bool has_value() const { return value_.has_value(); }
  [[nodiscard]] const OmsSnapshot& value() const {
    if (!value_.has_value()) {
      throw std::runtime_error("OmsSnapshotReadResult::value() called on a failed read: " +
                               std::string{describe(error_)});
    }
    return *value_;  // NOLINT(bugprone-unchecked-optional-access) - guarded above
  }
  [[nodiscard]] OmsSnapshot value_or(const OmsSnapshot& fallback) const {
    return value_.has_value() ? *value_ : fallback;
  }
  [[nodiscard]] OmsSnapshotError error() const { return error_; }

 private:
  std::optional<OmsSnapshot> value_;
  OmsSnapshotError error_{OmsSnapshotError::kTruncated};
};

[[nodiscard]] OmsSnapshotReadResult read_oms_snapshot(std::span<const std::byte> bytes);

/// Rebuilds each record into a `TrackedOrder` with a legally-constructed
/// `OrderLifecycle` at the record's saved state, ready to hand to
/// `OrderManager`'s restoring constructor.
[[nodiscard]] std::vector<TrackedOrder> to_tracked_orders(const OmsSnapshot& snapshot);

}  // namespace aegis::participant::oms
