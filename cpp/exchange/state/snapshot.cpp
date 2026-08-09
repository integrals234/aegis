#include "cpp/exchange/state/snapshot.hpp"

#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

#include "cpp/events/wire.hpp"

namespace aegis::exchange {
namespace {

using events::wire::put_i64;
using events::wire::put_u32;
using events::wire::put_u64;
using events::wire::put_u8;
using events::wire::take_i64;
using events::wire::take_u32;
using events::wire::take_u64;
using events::wire::take_u8;

void put_record(std::vector<std::byte>& out, const SnapshotOrderRecord& record) {
  put_u64(out, record.order_id);
  put_u32(out, record.instrument_id);
  put_u64(out, record.participant_id);
  put_u64(out, record.client_order_id);
  put_u8(out, static_cast<std::uint8_t>(record.side));
  put_u8(out, static_cast<std::uint8_t>(record.order_type));
  put_i64(out, record.price_units);
  put_i64(out, record.original_quantity);
  put_i64(out, record.cumulative_filled);
  put_i64(out, record.cancelled_quantity);
  put_i64(out, record.remaining);
  put_u64(out, record.priority_command_sequence);
}

[[nodiscard]] bool take_record(std::span<const std::byte> bytes, std::size_t& offset,
                               SnapshotOrderRecord& record) {
  std::uint8_t raw_side{0};
  std::uint8_t raw_order_type{0};
  if (!take_u64(bytes, offset, record.order_id) || !take_u32(bytes, offset, record.instrument_id) ||
      !take_u64(bytes, offset, record.participant_id) ||
      !take_u64(bytes, offset, record.client_order_id) || !take_u8(bytes, offset, raw_side) ||
      raw_side > static_cast<std::uint8_t>(Side::kSell) ||
      !take_u8(bytes, offset, raw_order_type) ||
      raw_order_type > static_cast<std::uint8_t>(OrderType::kMarket) ||
      !take_i64(bytes, offset, record.price_units) ||
      !take_i64(bytes, offset, record.original_quantity) ||
      !take_i64(bytes, offset, record.cumulative_filled) ||
      !take_i64(bytes, offset, record.cancelled_quantity) ||
      !take_i64(bytes, offset, record.remaining) ||
      !take_u64(bytes, offset, record.priority_command_sequence)) {
    return false;
  }
  record.side = static_cast<Side>(raw_side);
  record.order_type = static_cast<OrderType>(raw_order_type);
  return true;
}

/// A snapshot header counter must dominate its own contents, or restore
/// would hand out an identifier colliding with one already live in the
/// restored book (ADR-0013).
[[nodiscard]] bool counters_are_consistent(const ExchangeSnapshot& snapshot) {
  return std::ranges::all_of(snapshot.orders, [&snapshot](const SnapshotOrderRecord& record) {
    return snapshot.next_order_id > record.order_id &&
           snapshot.next_command_sequence > record.priority_command_sequence;
  });
}

}  // namespace

std::string_view describe(SnapshotError error) {
  switch (error) {
    case SnapshotError::kTruncated:
      return "snapshot bytes are truncated or carry trailing garbage";
    case SnapshotError::kUnknownVersion:
      return "snapshot_version is not a version this build understands";
    case SnapshotError::kCounterInconsistent:
      return "a header counter does not dominate its own restored contents";
  }
  return "unknown snapshot error";
}

ExchangeSnapshot capture_snapshot(const Sequencer& sequencer, const EventLog& event_log,
                                  std::uint64_t next_order_id,
                                  std::span<const OrderBook* const> books) {
  ExchangeSnapshot snapshot;
  snapshot.next_command_sequence = sequencer.next_command_sequence().value();
  snapshot.next_event_sequence = event_log.next_event_sequence().value();
  snapshot.next_order_id = next_order_id;
  snapshot.last_exchange_time_nanos = common::serialize_nanos(sequencer.last_exchange_time());

  for (const OrderBook* book : books) {
    for (const Side side : {Side::kBuy, Side::kSell}) {
      const auto& level_index = book->levels(side);
      std::optional<PriceUnits> price = level_index.best_price();
      while (price.has_value()) {
        for (const OrderId order_id : book->orders_at(side, *price)) {
          const OrderNode* node = book->find(order_id);
          snapshot.orders.push_back(SnapshotOrderRecord{
              .order_id = node->order_id.value(),
              .instrument_id = node->instrument_id.value(),
              .participant_id = node->participant_id.value(),
              .client_order_id = node->client_order_id.value(),
              .side = node->side,
              .order_type = node->order_type,
              .price_units = node->price_units.value(),
              .original_quantity = node->original_quantity.value(),
              .cumulative_filled = node->cumulative_filled.value(),
              .cancelled_quantity = node->cancelled_quantity.value(),
              .remaining = node->remaining.value(),
              .priority_command_sequence = node->priority.command_sequence().value(),
          });
        }
        price = level_index.next_price_after(*price);
      }
    }
  }

  // Canonical order (ADR-0013): (instrument_id, price_units, priority),
  // ascending. The per-book, per-side walk above does not produce this
  // globally (asks walk price ascending, bids walk price descending, and
  // multiple books interleave arbitrarily), so an explicit sort is what
  // actually establishes it.
  std::ranges::sort(
      snapshot.orders, [](const SnapshotOrderRecord& lhs, const SnapshotOrderRecord& rhs) {
        return std::tie(lhs.instrument_id, lhs.price_units, lhs.priority_command_sequence) <
               std::tie(rhs.instrument_id, rhs.price_units, rhs.priority_command_sequence);
      });

  return snapshot;
}

std::vector<std::byte> write_snapshot(const ExchangeSnapshot& snapshot) {
  std::vector<std::byte> out;
  put_u32(out, snapshot.snapshot_version);
  put_u64(out, snapshot.next_command_sequence);
  put_u64(out, snapshot.next_event_sequence);
  put_u64(out, snapshot.next_order_id);
  put_i64(out, snapshot.last_exchange_time_nanos);
  put_u64(out, snapshot.orders.size());
  for (const auto& record : snapshot.orders) {
    put_record(out, record);
  }
  return out;
}

SnapshotReadResult SnapshotReadResult::success(ExchangeSnapshot snapshot) {
  SnapshotReadResult result;
  result.value_ = std::move(snapshot);
  return result;
}

SnapshotReadResult SnapshotReadResult::failure(SnapshotError error) {
  SnapshotReadResult result;
  result.error_ = error;
  return result;
}

SnapshotReadResult read_snapshot(std::span<const std::byte> bytes) {
  ExchangeSnapshot snapshot;
  std::size_t offset = 0;
  std::uint64_t order_count = 0;
  if (!take_u32(bytes, offset, snapshot.snapshot_version) ||
      !take_u64(bytes, offset, snapshot.next_command_sequence) ||
      !take_u64(bytes, offset, snapshot.next_event_sequence) ||
      !take_u64(bytes, offset, snapshot.next_order_id) ||
      !take_i64(bytes, offset, snapshot.last_exchange_time_nanos) ||
      !take_u64(bytes, offset, order_count)) {
    return SnapshotReadResult::failure(SnapshotError::kTruncated);
  }
  if (snapshot.snapshot_version != kSnapshotVersion) {
    return SnapshotReadResult::failure(SnapshotError::kUnknownVersion);
  }

  snapshot.orders.reserve(order_count);
  for (std::uint64_t i = 0; i < order_count; ++i) {
    SnapshotOrderRecord record;
    if (!take_record(bytes, offset, record)) {
      return SnapshotReadResult::failure(SnapshotError::kTruncated);
    }
    snapshot.orders.push_back(record);
  }
  if (offset != bytes.size()) {
    return SnapshotReadResult::failure(SnapshotError::kTruncated);
  }

  if (!counters_are_consistent(snapshot)) {
    return SnapshotReadResult::failure(SnapshotError::kCounterInconsistent);
  }

  return SnapshotReadResult::success(std::move(snapshot));
}

void restore_orders_into(OrderBook& book, InstrumentId instrument_id,
                         const ExchangeSnapshot& snapshot) {
  for (const auto& record : snapshot.orders) {
    if (record.instrument_id != instrument_id.value()) {
      continue;
    }
    book.add(OrderNode{
        .order_id = OrderId{record.order_id},
        .instrument_id = InstrumentId{record.instrument_id},
        .participant_id = ParticipantId{record.participant_id},
        .client_order_id = ClientOrderId{record.client_order_id},
        .side = record.side,
        .order_type = record.order_type,
        .price_units = PriceUnits{record.price_units},
        .original_quantity = QuantityUnits{record.original_quantity},
        .cumulative_filled = QuantityUnits{record.cumulative_filled},
        .cancelled_quantity = QuantityUnits{record.cancelled_quantity},
        .remaining = QuantityUnits{record.remaining},
        .priority = Priority::from(CommandSequence{record.priority_command_sequence}),
    });
  }
}

}  // namespace aegis::exchange
