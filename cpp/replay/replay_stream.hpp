#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cpp/replay/replay_event.hpp"

/// Loading and validating a canonical replay input file (M2 slice 9,
/// AEGIS-058).
///
/// The one place that reads replay input from disk. `record_index` is
/// **read from the input, never recomputed** here or anywhere downstream
/// (M2 plan of record section 7) -- Python's `futures.ingest` is the only
/// place that assigns it. This module's job is narrower: load the four
/// canonical fields per record and refuse an input that is not already in
/// the one legal canonical order (`aegis::replay::canonical_less`,
/// slice 1), the same fail-closed discipline every other AEGIS loader in
/// this milestone follows.
namespace aegis::replay {

/// Reasons `load_replay_stream` refuses an input file. Returned rather than
/// thrown -- matches `events::DecodeResult`/`exchange::SnapshotReadResult`'s
/// existing pattern for "validate untrusted structured input, report why".
enum class ReplayStreamError : std::uint8_t {
  kFileNotFound,
  kMalformedRecord,
  kOutOfOrder,
  kDuplicateKey,
};

[[nodiscard]] std::string describe(ReplayStreamError error);

/// The outcome of `load_replay_stream`: either every record, in file order
/// (already validated canonical), or the reason loading failed.
///
/// A hand-rolled result type, matching `events::DecodeResult`'s own
/// documented reason: AEGIS targets C++20 (ADR-0005), and adopting
/// `std::expected` later would be a mechanical change to this same shape.
class ReplayStreamResult {
 public:
  static ReplayStreamResult success(std::vector<ReplayEvent> events);
  static ReplayStreamResult failure(ReplayStreamError error, std::string detail);

  [[nodiscard]] bool has_value() const { return value_.has_value(); }

  /// Precondition: has_value(). Throws std::runtime_error otherwise, so a
  /// caller that forgets to check fails loudly rather than reading a stream
  /// that was never loaded.
  [[nodiscard]] const std::vector<ReplayEvent>& value() const;

  [[nodiscard]] ReplayStreamError error() const { return error_; }
  [[nodiscard]] const std::string& detail() const { return detail_; }

 private:
  std::optional<std::vector<ReplayEvent>> value_;
  ReplayStreamError error_{ReplayStreamError::kFileNotFound};
  std::string detail_;
};

/// Load and validate a JSON-Lines replay stream from `path`. Each line is a
/// JSON object with `event_time_ns` (integer), `source_sequence` (integer),
/// `contract_symbol` (string) and `record_index` (integer) -- the same four
/// canonical fields `ReplayEvent` names, and the same field names
/// `python/futures/schema.py`'s `futures_bar.v1` uses for the equivalent
/// concepts, so a Python-produced manifest and a C++-loaded one describe the
/// same record without a field-name translation layer.
///
/// Rejects (never repairs): a missing/malformed field, a record that is not
/// canonically greater than the one before it (`kOutOfOrder`), and two
/// records comparing canonically equal (`kDuplicateKey` -- a `record_index`
/// collision, which should be impossible from correctly-assigned input and
/// is treated as a defect in the input, not silently deduplicated).
[[nodiscard]] ReplayStreamResult load_replay_stream(const std::string& path);

}  // namespace aegis::replay
