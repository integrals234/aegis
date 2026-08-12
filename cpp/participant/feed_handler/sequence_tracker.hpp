#pragma once

#include <cstdint>
#include <optional>

/// Market-data sequence validation (AEGIS-068; ADR-0021).
///
/// `SequenceTracker` is a pure function of the observed `md_sequence` stream:
/// it classifies what happened and updates its own last-seen value, and does
/// nothing else. Reacting to a gap (buffering, recovering) or to repeated
/// gaps (declaring the feed stale) is `BookBuilder`'s job, not this one's —
/// keeping diagnosis and reaction separate is what lets each be tested
/// without the other.
namespace aegis::participant::feed {

/// NOLINTNEXTLINE(performance-enum-size)
enum class SequenceDiagnostic : std::uint8_t {
  /// The first observation ever, or exactly one more than the last.
  kOk = 0,
  /// Strictly greater than one more than the last — one or more messages
  /// were never observed.
  kGap = 1,
  /// Exactly equal to the last observed value.
  kDuplicate = 2,
  /// Strictly less than the last observed value — the source restarted or
  /// rewound its own sequence numbering.
  kReset = 3,
};

struct SequenceCheckResult {
  SequenceDiagnostic diagnostic{SequenceDiagnostic::kOk};
  std::uint64_t observed_sequence{0};
  /// The sequence this observation would need to equal to continue without a
  /// gap. Meaningless (0) on the very first observation, since there is no
  /// "next expected" before anything has been seen.
  std::uint64_t expected_sequence{0};

  friend bool operator==(const SequenceCheckResult&, const SequenceCheckResult&) = default;
};

class SequenceTracker {
 public:
  SequenceTracker() = default;

  /// Classifies `sequence` against the last observation and records it as
  /// the new last observation regardless of the diagnostic — a duplicate or
  /// a reset still updates "what was most recently seen," so the next call
  /// is judged against what actually arrived, not against what would have
  /// been ideal.
  [[nodiscard]] SequenceCheckResult observe(std::uint64_t sequence);

  [[nodiscard]] std::optional<std::uint64_t> last_sequence() const { return last_; }

  /// Consecutive `kGap`/`kReset` diagnostics since the last `kOk`. Reset to 0
  /// on any `kOk` observation. `BookBuilder` reads this to decide when
  /// enough consecutive faults justify declaring the feed stale on its own,
  /// independent of the wall-clock threshold.
  [[nodiscard]] std::uint32_t consecutive_faults() const { return consecutive_faults_; }

  void reset();

 private:
  std::optional<std::uint64_t> last_;
  std::uint32_t consecutive_faults_{0};
};

}  // namespace aegis::participant::feed
