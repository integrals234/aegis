#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/replay/replay_event.hpp"
#include "cpp/replay/virtual_clock.hpp"

namespace aegis::replay {
class PacingPolicy;  // pacing.hpp -- forward-declared, only a reference is needed here
}  // namespace aegis::replay

/// Drives a validated, canonically-ordered replay stream through a
/// VirtualClock, one record (or one timestamp group) at a time, with
/// cursor/resume support (M2 slice 9, AEGIS-058).
///
/// Deliberately takes an already-loaded, already-validated vector rather
/// than owning file I/O itself: `load_replay_stream` (replay_stream.hpp) is
/// the one place that reads and validates an input file; this is the one
/// place that drives the validated result forward. Deterministic benchmark
/// mode is simply draining `next()` in a loop -- there is no pacing delay
/// to skip, because nothing here ever sleeps.
namespace aegis::replay {

class ReplayEngine {
 public:
  ReplayEngine(std::vector<ReplayEvent> events, VirtualClock& clock);

  ReplayEngine(const ReplayEngine&) = delete;
  ReplayEngine& operator=(const ReplayEngine&) = delete;
  ReplayEngine(ReplayEngine&&) = delete;
  ReplayEngine& operator=(ReplayEngine&&) = delete;
  ~ReplayEngine() = default;

  /// Emit the next record, advancing the clock to its event_time. Returns
  /// nullopt once every record has been emitted.
  [[nodiscard]] std::optional<ReplayEvent> next();

  /// Emit every remaining record sharing the same event_time_ns as the
  /// next one, as a single step (AEGIS-057's "one timestamp group").
  /// Returns an empty vector once every record has been emitted.
  [[nodiscard]] std::vector<ReplayEvent> next_group();

  /// Emit the next record together with the virtual wait `policy` computes
  /// before it, relative to the previously emitted record. The very first
  /// emission from this engine has no predecessor to compute a gap from, so
  /// its wait is always zero regardless of `policy` (AEGIS-054..057). The
  /// emitted event sequence is identical to plain `next()` -- pacing only
  /// changes the paired wait duration, never the event or its order.
  [[nodiscard]] std::optional<std::pair<ReplayEvent, common::Duration>> next_with_pacing(
      const PacingPolicy& policy);

  /// The record_index of the last emitted record, or nullopt if nothing has
  /// been emitted yet from this engine instance.
  [[nodiscard]] std::optional<RecordIndex> cursor() const;

  /// Seek so the next `next()`/`next_group()` call emits the record
  /// immediately after `index` in canonical order -- nothing at or before
  /// it is re-emitted. Throws std::invalid_argument if `index` does not
  /// name a record actually present in the loaded stream: resuming from a
  /// cursor the stream never produced is a caller bug, not a silent no-op.
  void resume_from(RecordIndex index);

  [[nodiscard]] bool has_next() const { return position_ < events_.size(); }
  [[nodiscard]] std::size_t size() const { return events_.size(); }

 private:
  std::vector<ReplayEvent> events_;
  VirtualClock* clock_;
  std::size_t position_{0};
};

}  // namespace aegis::replay
