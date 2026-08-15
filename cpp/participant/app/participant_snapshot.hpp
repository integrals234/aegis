#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/participant/oms/oms_snapshot.hpp"
#include "cpp/participant/portfolio/portfolio_snapshot.hpp"

/// Participant snapshot composition (AEGIS-237; ADR-0024): the process-
/// boundary recovery unit `aegis_participant_run --snapshot-out`/
/// `--restore-from` reads and writes.
///
/// `cpp-participant-app` is the only participant layer permitted to see the
/// OMS and portfolio at once (ADR-0020), so it is the only layer that may
/// compose their two independently-owned snapshot codecs into one file.
/// Each component's own bytes are produced and validated entirely by its
/// owning module (`oms::write_oms_snapshot`/`read_oms_snapshot`,
/// `portfolio::write_portfolio_snapshot`/`read_portfolio_snapshot`) --
/// this header only concatenates two length-prefixed, already-self-
/// contained blobs behind one outer version. No field of either component
/// snapshot is read or interpreted here (`docs/RECOVERY_CONTRACT.md`
/// obligation 1: a snapshot covers exactly the state its owning module is
/// responsible for). Book-builder/market-data reconstruction state is
/// deliberately absent: that is AEGIS-070's in-stream recovery, a different
/// mechanism for a different question (ADR-0024).
namespace aegis::participant::app {

inline constexpr std::uint32_t kParticipantSnapshotVersion = 1;

struct ParticipantSnapshot {
  std::uint32_t snapshot_version{kParticipantSnapshotVersion};
  oms::OmsSnapshot oms;
  portfolio::PortfolioSnapshot portfolio;

  friend bool operator==(const ParticipantSnapshot&, const ParticipantSnapshot&) = default;
};

[[nodiscard]] ParticipantSnapshot capture_participant_snapshot(const oms::OrderManager& manager,
                                                               const portfolio::Portfolio& ledger);

/// Byte-stable canonical encoding: `u32` outer version, then each
/// component's own byte-stable encoding, each preceded by its own `u64`
/// length prefix so the outer reader can hand each component reader
/// exactly its own bytes without either needing to know the other's format.
[[nodiscard]] std::vector<std::byte> write_participant_snapshot(
    const ParticipantSnapshot& snapshot);

enum class ParticipantSnapshotError : std::uint8_t {
  kTruncated,
  /// The outer `snapshot_version` is not `kParticipantSnapshotVersion`.
  kUnknownVersion,
  /// A component blob failed its own codec's validation (unknown component
  /// version, truncation within the component, or a component-specific
  /// consistency failure) -- the outer error does not distinguish which
  /// component or why; `read_participant_snapshot`'s caller that needs that
  /// detail can re-run the component reader directly on the same bytes.
  kComponentInvalid,
};

[[nodiscard]] std::string_view describe(ParticipantSnapshotError error);

class ParticipantSnapshotReadResult {
 public:
  [[nodiscard]] static ParticipantSnapshotReadResult success(ParticipantSnapshot snapshot);
  [[nodiscard]] static ParticipantSnapshotReadResult failure(ParticipantSnapshotError error);

  [[nodiscard]] bool has_value() const { return value_.has_value(); }
  [[nodiscard]] const ParticipantSnapshot& value() const {
    if (!value_.has_value()) {
      throw std::runtime_error("ParticipantSnapshotReadResult::value() called on a failed read: " +
                               std::string{describe(error_)});
    }
    return *value_;  // NOLINT(bugprone-unchecked-optional-access) - guarded above
  }
  [[nodiscard]] ParticipantSnapshot value_or(const ParticipantSnapshot& fallback) const {
    return value_.has_value() ? *value_ : fallback;
  }
  [[nodiscard]] ParticipantSnapshotError error() const { return error_; }

 private:
  std::optional<ParticipantSnapshot> value_;
  ParticipantSnapshotError error_{ParticipantSnapshotError::kTruncated};
};

[[nodiscard]] ParticipantSnapshotReadResult read_participant_snapshot(
    std::span<const std::byte> bytes);

}  // namespace aegis::participant::app
