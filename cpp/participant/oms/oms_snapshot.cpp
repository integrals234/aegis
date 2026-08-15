#include "cpp/participant/oms/oms_snapshot.hpp"

#include <algorithm>
#include <utility>

#include "cpp/events/wire.hpp"

namespace aegis::participant::oms {
namespace {

using events::exchange::Side;
using events::wire::put_i64;
using events::wire::put_u32;
using events::wire::put_u64;
using events::wire::put_u8;
using events::wire::take_i64;
using events::wire::take_u32;
using events::wire::take_u64;
using events::wire::take_u8;

constexpr std::uint8_t kMaxOrderState = static_cast<std::uint8_t>(OrderState::kExpired);

void put_record(std::vector<std::byte>& out, const OmsOrderRecord& record) {
  put_u64(out, record.client_order_id);
  put_u64(out, record.exchange_order_id);
  put_u32(out, record.instrument_id);
  put_u64(out, record.participant_id);
  put_u8(out, static_cast<std::uint8_t>(record.side));
  put_u8(out, record.lifecycle_state);
  put_i64(out, record.price_units);
  put_i64(out, record.original_quantity_units);
  put_i64(out, record.cumulative_filled_units);
  put_i64(out, record.remaining_units);
  put_i64(out, record.cumulative_fees_units);
  put_i64(out, record.cumulative_slippage_cost_units);
}

[[nodiscard]] bool take_record(std::span<const std::byte> bytes, std::size_t& offset,
                               OmsOrderRecord& record) {
  std::uint8_t raw_side{0};
  if (!take_u64(bytes, offset, record.client_order_id) ||
      !take_u64(bytes, offset, record.exchange_order_id) ||
      !take_u32(bytes, offset, record.instrument_id) ||
      !take_u64(bytes, offset, record.participant_id) || !take_u8(bytes, offset, raw_side) ||
      raw_side > static_cast<std::uint8_t>(Side::kSell) ||
      !take_u8(bytes, offset, record.lifecycle_state) || record.lifecycle_state > kMaxOrderState ||
      !take_i64(bytes, offset, record.price_units) ||
      !take_i64(bytes, offset, record.original_quantity_units) ||
      !take_i64(bytes, offset, record.cumulative_filled_units) ||
      !take_i64(bytes, offset, record.remaining_units) ||
      !take_i64(bytes, offset, record.cumulative_fees_units) ||
      !take_i64(bytes, offset, record.cumulative_slippage_cost_units)) {
    return false;
  }
  record.side = static_cast<Side>(raw_side);
  return true;
}

/// Mirrors `exchange::counters_are_consistent`: the header counter must
/// dominate its own contents, or restore would hand out a client_order_id
/// colliding with one already tracked.
[[nodiscard]] bool counters_are_consistent(const OmsSnapshot& snapshot) {
  return std::ranges::all_of(snapshot.orders, [&snapshot](const OmsOrderRecord& record) {
    return snapshot.next_client_order_id > record.client_order_id;
  });
}

}  // namespace

std::string_view describe(OmsSnapshotError error) {
  switch (error) {
    case OmsSnapshotError::kTruncated:
      return "oms snapshot bytes are truncated or carry trailing garbage";
    case OmsSnapshotError::kUnknownVersion:
      return "oms snapshot_version is not a version this build understands";
    case OmsSnapshotError::kCounterInconsistent:
      return "a header counter does not dominate its own restored contents";
  }
  return "unknown oms snapshot error";
}

OmsSnapshot capture_oms_snapshot(const OrderManager& manager) {
  OmsSnapshot snapshot;
  snapshot.next_client_order_id = manager.next_client_order_id();
  for (const TrackedOrder& tracked : manager.all_tracked_orders()) {
    snapshot.orders.push_back(OmsOrderRecord{
        .client_order_id = tracked.client_order_id,
        .exchange_order_id = tracked.exchange_order_id,
        .instrument_id = tracked.instrument_id,
        .participant_id = tracked.participant_id,
        .side = tracked.side,
        .lifecycle_state = static_cast<std::uint8_t>(tracked.lifecycle.state()),
        .price_units = tracked.price_units,
        .original_quantity_units = tracked.original_quantity_units,
        .cumulative_filled_units = tracked.cumulative_filled_units,
        .remaining_units = tracked.remaining_units,
        .cumulative_fees_units = tracked.cumulative_fees_units,
        .cumulative_slippage_cost_units = tracked.cumulative_slippage_cost_units,
    });
  }
  // all_tracked_orders() already returns ascending client_order_id order;
  // no further sort needed here (unlike ExchangeSnapshot, which merges
  // several books' independent walks and must sort explicitly).
  return snapshot;
}

std::vector<std::byte> write_oms_snapshot(const OmsSnapshot& snapshot) {
  std::vector<std::byte> out;
  put_u32(out, snapshot.snapshot_version);
  put_u64(out, snapshot.next_client_order_id);
  put_u64(out, snapshot.orders.size());
  for (const auto& record : snapshot.orders) {
    put_record(out, record);
  }
  return out;
}

OmsSnapshotReadResult OmsSnapshotReadResult::success(OmsSnapshot snapshot) {
  OmsSnapshotReadResult result;
  result.value_ = std::move(snapshot);
  return result;
}

OmsSnapshotReadResult OmsSnapshotReadResult::failure(OmsSnapshotError error) {
  OmsSnapshotReadResult result;
  result.error_ = error;
  return result;
}

OmsSnapshotReadResult read_oms_snapshot(std::span<const std::byte> bytes) {
  OmsSnapshot snapshot;
  std::size_t offset = 0;
  std::uint64_t order_count = 0;
  if (!take_u32(bytes, offset, snapshot.snapshot_version) ||
      !take_u64(bytes, offset, snapshot.next_client_order_id) ||
      !take_u64(bytes, offset, order_count)) {
    return OmsSnapshotReadResult::failure(OmsSnapshotError::kTruncated);
  }
  if (snapshot.snapshot_version != kOmsSnapshotVersion) {
    return OmsSnapshotReadResult::failure(OmsSnapshotError::kUnknownVersion);
  }

  snapshot.orders.reserve(order_count);
  for (std::uint64_t i = 0; i < order_count; ++i) {
    OmsOrderRecord record;
    if (!take_record(bytes, offset, record)) {
      return OmsSnapshotReadResult::failure(OmsSnapshotError::kTruncated);
    }
    snapshot.orders.push_back(record);
  }
  if (offset != bytes.size()) {
    return OmsSnapshotReadResult::failure(OmsSnapshotError::kTruncated);
  }

  if (!counters_are_consistent(snapshot)) {
    return OmsSnapshotReadResult::failure(OmsSnapshotError::kCounterInconsistent);
  }

  return OmsSnapshotReadResult::success(std::move(snapshot));
}

std::vector<TrackedOrder> to_tracked_orders(const OmsSnapshot& snapshot) {
  std::vector<TrackedOrder> orders;
  orders.reserve(snapshot.orders.size());
  for (const auto& record : snapshot.orders) {
    orders.push_back(TrackedOrder{
        .lifecycle = OrderLifecycle{static_cast<OrderState>(record.lifecycle_state)},
        .client_order_id = record.client_order_id,
        .exchange_order_id = record.exchange_order_id,
        .instrument_id = record.instrument_id,
        .participant_id = record.participant_id,
        .side = record.side,
        .price_units = record.price_units,
        .original_quantity_units = record.original_quantity_units,
        .cumulative_filled_units = record.cumulative_filled_units,
        .remaining_units = record.remaining_units,
        .cumulative_fees_units = record.cumulative_fees_units,
        .cumulative_slippage_cost_units = record.cumulative_slippage_cost_units,
        // Derived from a market event, not persisted state: a restored order
        // has no event to attribute, and inventing one would fabricate a
        // measurement (AEGIS-113).
        .latency = std::nullopt,
    });
  }
  return orders;
}

}  // namespace aegis::participant::oms
