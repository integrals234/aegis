#pragma once

#include "cpp/common/clock.hpp"
#include "cpp/common/time.hpp"

/// A wall clock driven entirely by replay event advancement (M2 slice 9,
/// AEGIS-058).
///
/// `now_utc()` never reads the system clock and this class never sleeps --
/// that absence is structural, not a flag a caller can misconfigure, which
/// is what "deterministic benchmark mode... without wall-clock sleeps"
/// means concretely: a consumer that asks this clock the time sees the
/// replayed stream's own event time, and advancing it costs nothing but a
/// comparison and an assignment.
namespace aegis::replay {

class VirtualClock final : public common::WallClock {
 public:
  explicit VirtualClock(common::Nanos start = 0) : now_(start) {}

  [[nodiscard]] common::Nanos now_utc() const override { return now_; }

  /// Advance to `time`. Refuses to move backward: `replay_stream.hpp`
  /// validates every loaded stream is in non-decreasing canonical order
  /// before it ever reaches a clock, so a caller asking this clock to go
  /// backward is a bug at the call site, not a legitimate replay of
  /// out-of-order data.
  void advance_to(common::EventTime time);

 private:
  common::Nanos now_;
};

}  // namespace aegis::replay
