#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/replay/replay_event.hpp"

/// Deterministic fault injection over an already-validated replay stream
/// (M2 slices 11-12, AEGIS-060, 061, 062, 063).
///
/// This is the injection **mechanism only** -- a pure, deterministic
/// function from a stream and an explicit rule set to a fault-annotated
/// result. It does not simulate a response to any fault: AEGIS-060's
/// "stale-data response" and AEGIS-061's "recovery" need a participant-side
/// consumer that does not exist before M3; AEGIS-062's "risk response" and
/// AEGIS-063's "OMS/risk integration" need the risk/OMS layers that do not
/// exist before M5. Building any of those responses here would be
/// implementing a later milestone's subsystem early, which the M2 plan of
/// record explicitly forbids.
///
/// Every fault is **deterministic by construction**: `FaultRule` is
/// explicit, caller-supplied data (which `record_index` a fault targets,
/// what kind, what parameters) -- never a seeded-random selection the
/// caller has to trust "probably" behaves the same way twice. No fault
/// mutates a record's own canonical-order fields (`event_time`,
/// `source_sequence`, `contract_symbol`, `record_index`); every fault is
/// either a pure annotation alongside the untouched record, an extra
/// duplicate copy, or an accounted-for omission -- so the total order this
/// milestone spent three slices establishing is never at risk from fault
/// injection itself.
namespace aegis::replay {

/// AEGIS-060/061 (slice 11): kDelayed, kMissing, kDuplicated, kSequenceGap.
/// AEGIS-062/063 (slice 12) extend this same enum rather than introducing a
/// second injector -- one mechanism, more kinds.
enum class FaultKind : std::uint8_t {
  kDelayed,
  kMissing,
  kDuplicated,
  kSequenceGap,
};

[[nodiscard]] std::string describe(FaultKind kind);

/// One explicit, deterministic fault: apply `kind` to the record whose
/// `record_index` is `target`. `delay` is meaningful only for `kDelayed`;
/// `magnitude` only for `kSequenceGap` (the size of the simulated source-
/// sequence gap) and the slice-12 stress kinds. At most one rule may target
/// a given `record_index` -- `apply` rejects an ambiguous rule set rather
/// than silently picking one.
struct FaultRule {
  RecordIndex target;
  FaultKind kind{FaultKind::kDelayed};
  common::Duration delay;
  std::uint64_t magnitude{0};
};

/// Attached to a surviving or duplicated record to declare which fault (if
/// any) affected it. A record with no rule targeting it carries no
/// annotation at all (`std::nullopt` in `FaultInjectionResult::events`),
/// not a "kind: none" sentinel -- absence of a fault is absence of an
/// annotation, not a fourth state to keep in sync with the other three.
struct FaultAnnotation {
  FaultKind kind{FaultKind::kDelayed};
  common::Duration delay;
  std::uint64_t magnitude{0};

  friend bool operator==(const FaultAnnotation&, const FaultAnnotation&) = default;
};

/// A record `kMissing` (or a slice-12 kind that also omits its record)
/// removed from the emitted sequence -- recorded here so nothing is lost
/// silently, even though it is genuinely absent from `events`.
struct DroppedRecord {
  RecordIndex record_index;
  FaultKind reason{FaultKind::kMissing};
};

struct FaultInjectionResult {
  /// In canonical order (the same order `input` arrived in, since no fault
  /// reorders anything): each surviving or duplicated record, paired with
  /// its annotation if one applies.
  std::vector<std::pair<ReplayEvent, std::optional<FaultAnnotation>>> events;
  std::vector<DroppedRecord> dropped;
};

class DeterministicFaultInjector {
 public:
  /// Applies `rules` to `input` (assumed already canonically validated by
  /// `load_replay_stream`). Throws std::invalid_argument if two rules
  /// target the same `record_index` -- an ambiguous rule set is a caller
  /// error, not something to resolve by picking the first one seen.
  [[nodiscard]] static FaultInjectionResult apply(const std::vector<ReplayEvent>& input,
                                                  const std::vector<FaultRule>& rules);
};

}  // namespace aegis::replay
