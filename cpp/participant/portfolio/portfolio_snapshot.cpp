#include "cpp/participant/portfolio/portfolio_snapshot.hpp"

#include <unordered_set>
#include <utility>

#include "cpp/events/wire.hpp"

namespace aegis::participant::portfolio {
namespace {

using events::wire::put_i64;
using events::wire::put_u32;
using events::wire::put_u64;
using events::wire::take_i64;
using events::wire::take_u32;
using events::wire::take_u64;

void put_record(std::vector<std::byte>& out, const PortfolioPositionRecord& record) {
  put_u32(out, record.instrument_id);
  put_i64(out, record.quantity_units);
  put_i64(out, record.average_price_units);
  put_i64(out, record.realized_pnl_units);
}

[[nodiscard]] bool take_record(std::span<const std::byte> bytes, std::size_t& offset,
                               PortfolioPositionRecord& record) {
  return take_u32(bytes, offset, record.instrument_id) &&
         take_i64(bytes, offset, record.quantity_units) &&
         take_i64(bytes, offset, record.average_price_units) &&
         take_i64(bytes, offset, record.realized_pnl_units);
}

[[nodiscard]] bool instruments_are_unique(const PortfolioSnapshot& snapshot) {
  std::unordered_set<std::uint32_t> seen;
  seen.reserve(snapshot.positions.size());
  for (const auto& record : snapshot.positions) {
    if (!seen.insert(record.instrument_id).second) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string_view describe(PortfolioSnapshotError error) {
  switch (error) {
    case PortfolioSnapshotError::kTruncated:
      return "portfolio snapshot bytes are truncated or carry trailing garbage";
    case PortfolioSnapshotError::kUnknownVersion:
      return "portfolio snapshot_version is not a version this build understands";
    case PortfolioSnapshotError::kDuplicateInstrument:
      return "two records in the portfolio snapshot name the same instrument_id";
  }
  return "unknown portfolio snapshot error";
}

PortfolioSnapshot capture_portfolio_snapshot(const Portfolio& ledger) {
  PortfolioSnapshot snapshot;
  snapshot.cash_units = ledger.cash_units();
  for (const auto& [instrument_id, position] : ledger.all_positions()) {
    snapshot.positions.push_back(PortfolioPositionRecord{
        .instrument_id = instrument_id,
        .quantity_units = position.quantity_units,
        .average_price_units = position.average_price_units,
        .realized_pnl_units = position.realized_pnl_units,
    });
  }
  // all_positions() already returns ascending instrument_id order.
  return snapshot;
}

std::vector<std::byte> write_portfolio_snapshot(const PortfolioSnapshot& snapshot) {
  std::vector<std::byte> out;
  put_u32(out, snapshot.snapshot_version);
  put_i64(out, snapshot.cash_units);
  put_u64(out, snapshot.positions.size());
  for (const auto& record : snapshot.positions) {
    put_record(out, record);
  }
  return out;
}

PortfolioSnapshotReadResult PortfolioSnapshotReadResult::success(PortfolioSnapshot snapshot) {
  PortfolioSnapshotReadResult result;
  result.value_ = std::move(snapshot);
  return result;
}

PortfolioSnapshotReadResult PortfolioSnapshotReadResult::failure(PortfolioSnapshotError error) {
  PortfolioSnapshotReadResult result;
  result.error_ = error;
  return result;
}

PortfolioSnapshotReadResult read_portfolio_snapshot(std::span<const std::byte> bytes) {
  PortfolioSnapshot snapshot;
  std::size_t offset = 0;
  std::uint64_t position_count = 0;
  if (!take_u32(bytes, offset, snapshot.snapshot_version) ||
      !take_i64(bytes, offset, snapshot.cash_units) || !take_u64(bytes, offset, position_count)) {
    return PortfolioSnapshotReadResult::failure(PortfolioSnapshotError::kTruncated);
  }
  if (snapshot.snapshot_version != kPortfolioSnapshotVersion) {
    return PortfolioSnapshotReadResult::failure(PortfolioSnapshotError::kUnknownVersion);
  }

  snapshot.positions.reserve(position_count);
  for (std::uint64_t i = 0; i < position_count; ++i) {
    PortfolioPositionRecord record;
    if (!take_record(bytes, offset, record)) {
      return PortfolioSnapshotReadResult::failure(PortfolioSnapshotError::kTruncated);
    }
    snapshot.positions.push_back(record);
  }
  if (offset != bytes.size()) {
    return PortfolioSnapshotReadResult::failure(PortfolioSnapshotError::kTruncated);
  }

  if (!instruments_are_unique(snapshot)) {
    return PortfolioSnapshotReadResult::failure(PortfolioSnapshotError::kDuplicateInstrument);
  }

  return PortfolioSnapshotReadResult::success(std::move(snapshot));
}

Portfolio restore_portfolio(const PortfolioSnapshot& snapshot) {
  std::vector<std::pair<std::uint32_t, Position>> positions;
  positions.reserve(snapshot.positions.size());
  for (const auto& record : snapshot.positions) {
    positions.emplace_back(record.instrument_id,
                           Position{.quantity_units = record.quantity_units,
                                    .average_price_units = record.average_price_units,
                                    .realized_pnl_units = record.realized_pnl_units});
  }
  return Portfolio{snapshot.cash_units, std::move(positions)};
}

}  // namespace aegis::participant::portfolio
