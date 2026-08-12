#include "cpp/events/market_data_messages.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "cpp/events/wire.hpp"

namespace aegis::events::market_data {
namespace {

using wire::put_i64;
using wire::put_u32;
using wire::put_u64;
using wire::put_u8;
using wire::take_i64;
using wire::take_u32;
using wire::take_u64;
using wire::take_u8;

void put_side(std::vector<std::byte>& out, exchange::Side side) {
  put_u8(out, static_cast<std::uint8_t>(side));
}

[[nodiscard]] bool take_side(std::span<const std::byte> bytes, std::size_t& offset,
                             exchange::Side& out) {
  std::uint8_t raw{0};
  if (!take_u8(bytes, offset, raw) || raw > static_cast<std::uint8_t>(exchange::Side::kSell)) {
    return false;
  }
  out = static_cast<exchange::Side>(raw);
  return true;
}

void put_entry(std::vector<std::byte>& out, const BookLevelEntry& entry) {
  put_side(out, entry.side);
  put_i64(out, entry.price_units);
  put_i64(out, entry.quantity_units);
  put_u64(out, entry.order_id);
}

[[nodiscard]] bool take_entry(std::span<const std::byte> bytes, std::size_t& offset,
                              BookLevelEntry& out) {
  return take_side(bytes, offset, out.side) && take_i64(bytes, offset, out.price_units) &&
         take_i64(bytes, offset, out.quantity_units) && take_u64(bytes, offset, out.order_id);
}

}  // namespace

bool is_known_delta_kind(std::uint8_t value) {
  return value <= static_cast<std::uint8_t>(DeltaKind::kPriceLevelSet);
}

std::vector<std::byte> encode(const BookSnapshotEvent& event) {
  std::vector<std::byte> out;
  put_u32(out, event.instrument_id);
  put_u64(out, event.md_sequence);
  // Length-prefixed repeated field, same convention as wire::put_string.
  put_u64(out, event.entries.size());
  for (const BookLevelEntry& entry : event.entries) {
    put_entry(out, entry);
  }
  return out;
}

std::optional<BookSnapshotEvent> decode_book_snapshot(std::span<const std::byte> bytes) {
  BookSnapshotEvent event;
  std::size_t offset = 0;
  std::uint64_t entry_count{0};
  if (!take_u32(bytes, offset, event.instrument_id) ||
      !take_u64(bytes, offset, event.md_sequence) || !take_u64(bytes, offset, entry_count)) {
    return std::nullopt;
  }
  event.entries.reserve(entry_count);
  for (std::uint64_t i = 0; i < entry_count; ++i) {
    BookLevelEntry entry;
    if (!take_entry(bytes, offset, entry)) {
      return std::nullopt;
    }
    event.entries.push_back(entry);
  }
  if (offset != bytes.size()) {
    return std::nullopt;
  }
  return event;
}

std::vector<std::byte> encode(const BookDeltaEvent& event) {
  std::vector<std::byte> out;
  put_u32(out, event.instrument_id);
  put_u64(out, event.md_sequence);
  put_u8(out, static_cast<std::uint8_t>(event.kind));
  put_u64(out, event.order_id);
  put_side(out, event.side);
  put_i64(out, event.price_units);
  put_i64(out, event.quantity_units);
  return out;
}

std::optional<BookDeltaEvent> decode_book_delta(std::span<const std::byte> bytes) {
  BookDeltaEvent event;
  std::size_t offset = 0;
  std::uint8_t raw_kind{0};
  if (!take_u32(bytes, offset, event.instrument_id) ||
      !take_u64(bytes, offset, event.md_sequence) || !take_u8(bytes, offset, raw_kind) ||
      !is_known_delta_kind(raw_kind) || !take_u64(bytes, offset, event.order_id) ||
      !take_side(bytes, offset, event.side) || !take_i64(bytes, offset, event.price_units) ||
      !take_i64(bytes, offset, event.quantity_units) || offset != bytes.size()) {
    return std::nullopt;
  }
  event.kind = static_cast<DeltaKind>(raw_kind);
  return event;
}

}  // namespace aegis::events::market_data
