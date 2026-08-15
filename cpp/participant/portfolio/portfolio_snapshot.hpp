#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/participant/portfolio/portfolio.hpp"

/// Portfolio snapshot codec (AEGIS-237; ADR-0024): the on-wire form of cash
/// and every instrument's position `Portfolio` holds.
///
/// `cpp-participant-portfolio` owns this codec because it owns the state
/// inside it (`docs/RECOVERY_CONTRACT.md` obligation 1) -- this header
/// knows nothing about OMS state or about how the app layer combines this
/// snapshot with the OMS's own. Mirrors `cpp/exchange/state/snapshot.hpp`
/// (ADR-0013) and `cpp/participant/oms/oms_snapshot.hpp` in shape and
/// discipline: byte-stable, version-refusing, round-trip verified.
namespace aegis::participant::portfolio {

inline constexpr std::uint32_t kPortfolioSnapshotVersion = 1;

struct PortfolioPositionRecord {
  std::uint32_t instrument_id{0};
  std::int64_t quantity_units{0};
  std::int64_t average_price_units{0};
  std::int64_t realized_pnl_units{0};

  friend bool operator==(const PortfolioPositionRecord&, const PortfolioPositionRecord&) = default;
};

struct PortfolioSnapshot {
  std::uint32_t snapshot_version{kPortfolioSnapshotVersion};
  std::int64_t cash_units{0};
  /// Canonical order: ascending `instrument_id` (matches
  /// `Portfolio::all_positions()`).
  std::vector<PortfolioPositionRecord> positions;

  friend bool operator==(const PortfolioSnapshot&, const PortfolioSnapshot&) = default;
};

[[nodiscard]] PortfolioSnapshot capture_portfolio_snapshot(const Portfolio& ledger);

/// Byte-stable canonical encoding (`cpp/events/wire.hpp` primitives): fixed
/// field order, fixed-width little-endian, no floating point.
[[nodiscard]] std::vector<std::byte> write_portfolio_snapshot(const PortfolioSnapshot& snapshot);

enum class PortfolioSnapshotError : std::uint8_t {
  kTruncated,
  /// `snapshot_version` is not `kPortfolioSnapshotVersion`.
  kUnknownVersion,
  /// Two records name the same `instrument_id` -- an ambiguous restore,
  /// since `Portfolio`'s restoring constructor can only hold one position
  /// per instrument.
  kDuplicateInstrument,
};

[[nodiscard]] std::string_view describe(PortfolioSnapshotError error);

class PortfolioSnapshotReadResult {
 public:
  [[nodiscard]] static PortfolioSnapshotReadResult success(PortfolioSnapshot snapshot);
  [[nodiscard]] static PortfolioSnapshotReadResult failure(PortfolioSnapshotError error);

  [[nodiscard]] bool has_value() const { return value_.has_value(); }
  [[nodiscard]] const PortfolioSnapshot& value() const {
    if (!value_.has_value()) {
      throw std::runtime_error("PortfolioSnapshotReadResult::value() called on a failed read: " +
                               std::string{describe(error_)});
    }
    return *value_;  // NOLINT(bugprone-unchecked-optional-access) - guarded above
  }
  [[nodiscard]] PortfolioSnapshot value_or(const PortfolioSnapshot& fallback) const {
    return value_.has_value() ? *value_ : fallback;
  }
  [[nodiscard]] PortfolioSnapshotError error() const { return error_; }

 private:
  std::optional<PortfolioSnapshot> value_;
  PortfolioSnapshotError error_{PortfolioSnapshotError::kTruncated};
};

[[nodiscard]] PortfolioSnapshotReadResult read_portfolio_snapshot(std::span<const std::byte> bytes);

/// Rebuilds a `Portfolio` directly from `snapshot`, via the restoring
/// constructor -- no fill is replayed.
[[nodiscard]] Portfolio restore_portfolio(const PortfolioSnapshot& snapshot);

}  // namespace aegis::participant::portfolio
