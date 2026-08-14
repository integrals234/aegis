#include "cpp/participant/app/participant_snapshot.hpp"

#include <utility>

#include "cpp/events/wire.hpp"

namespace aegis::participant::app {
namespace {

using events::wire::put_u32;
using events::wire::put_u64;
using events::wire::take_u32;
using events::wire::take_u64;

}  // namespace

std::string_view describe(ParticipantSnapshotError error) {
  switch (error) {
    case ParticipantSnapshotError::kTruncated:
      return "participant snapshot bytes are truncated or carry trailing garbage";
    case ParticipantSnapshotError::kUnknownVersion:
      return "participant snapshot_version is not a version this build understands";
    case ParticipantSnapshotError::kComponentInvalid:
      return "a component (oms or portfolio) snapshot failed its own codec's validation";
  }
  return "unknown participant snapshot error";
}

ParticipantSnapshot capture_participant_snapshot(const oms::OrderManager& manager,
                                                 const portfolio::Portfolio& ledger) {
  return ParticipantSnapshot{
      .oms = oms::capture_oms_snapshot(manager),
      .portfolio = portfolio::capture_portfolio_snapshot(ledger),
  };
}

std::vector<std::byte> write_participant_snapshot(const ParticipantSnapshot& snapshot) {
  std::vector<std::byte> out;
  put_u32(out, snapshot.snapshot_version);

  const auto oms_bytes = oms::write_oms_snapshot(snapshot.oms);
  put_u64(out, oms_bytes.size());
  out.insert(out.end(), oms_bytes.begin(), oms_bytes.end());

  const auto portfolio_bytes = portfolio::write_portfolio_snapshot(snapshot.portfolio);
  put_u64(out, portfolio_bytes.size());
  out.insert(out.end(), portfolio_bytes.begin(), portfolio_bytes.end());

  return out;
}

ParticipantSnapshotReadResult ParticipantSnapshotReadResult::success(ParticipantSnapshot snapshot) {
  ParticipantSnapshotReadResult result;
  result.value_ = std::move(snapshot);
  return result;
}

ParticipantSnapshotReadResult ParticipantSnapshotReadResult::failure(
    ParticipantSnapshotError error) {
  ParticipantSnapshotReadResult result;
  result.error_ = error;
  return result;
}

ParticipantSnapshotReadResult read_participant_snapshot(std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  std::uint32_t snapshot_version = 0;
  std::uint64_t oms_length = 0;
  if (!take_u32(bytes, offset, snapshot_version) || !take_u64(bytes, offset, oms_length)) {
    return ParticipantSnapshotReadResult::failure(ParticipantSnapshotError::kTruncated);
  }
  if (snapshot_version != kParticipantSnapshotVersion) {
    return ParticipantSnapshotReadResult::failure(ParticipantSnapshotError::kUnknownVersion);
  }
  if (offset + oms_length > bytes.size()) {
    return ParticipantSnapshotReadResult::failure(ParticipantSnapshotError::kTruncated);
  }
  const auto oms_result = oms::read_oms_snapshot(bytes.subspan(offset, oms_length));
  if (!oms_result.has_value()) {
    return ParticipantSnapshotReadResult::failure(ParticipantSnapshotError::kComponentInvalid);
  }
  offset += oms_length;

  std::uint64_t portfolio_length = 0;
  if (!take_u64(bytes, offset, portfolio_length)) {
    return ParticipantSnapshotReadResult::failure(ParticipantSnapshotError::kTruncated);
  }
  if (offset + portfolio_length > bytes.size()) {
    return ParticipantSnapshotReadResult::failure(ParticipantSnapshotError::kTruncated);
  }
  const auto portfolio_result =
      portfolio::read_portfolio_snapshot(bytes.subspan(offset, portfolio_length));
  if (!portfolio_result.has_value()) {
    return ParticipantSnapshotReadResult::failure(ParticipantSnapshotError::kComponentInvalid);
  }
  offset += portfolio_length;

  if (offset != bytes.size()) {
    return ParticipantSnapshotReadResult::failure(ParticipantSnapshotError::kTruncated);
  }

  return ParticipantSnapshotReadResult::success(ParticipantSnapshot{
      .snapshot_version = snapshot_version,
      .oms = oms_result.value(),
      .portfolio = portfolio_result.value(),
  });
}

}  // namespace aegis::participant::app
