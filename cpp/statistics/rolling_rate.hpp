#pragma once

#include <cstddef>
#include <deque>

#include "cpp/common/time.hpp"

/// Generic online event-rate estimator (AEGIS-074; ADR-0022).
///
/// Same discipline as `RollingMoments`: `cpp-statistics.may_depend_on =
/// [cpp-common]` only, so this class knows nothing of trades, cancellations,
/// or any other domain concept -- a caller decides what an "event" is and
/// supplies only its timestamp.
namespace aegis::participant::stats {

/// Counts events falling within a trailing time window, evaluated as of the
/// most recently recorded event. Eviction happens lazily on `record_event`,
/// using that event's own timestamp as "now" -- there is deliberately no
/// separate `evict(now)` that would make a `const` query mutate state.
class RollingRate {
 public:
  /// Precondition: `window.nanos() > 0`.
  explicit RollingRate(common::Duration window) : window_(window) {}

  /// Records one event at `timestamp_nanos`, then evicts every earlier
  /// timestamp that has fallen outside `window` as of this one.
  /// Precondition: `timestamp_nanos` is non-decreasing across calls --
  /// eviction assumes events arrive in time order, matching every other
  /// deterministic component in this codebase.
  void record_event(common::Nanos timestamp_nanos);

  /// The number of events currently in the window, as of the last
  /// `record_event` call.
  [[nodiscard]] std::size_t count() const { return timestamps_.size(); }

  /// `count() / window` in events per second. `0.0` with no events recorded.
  [[nodiscard]] double rate_per_second() const;

 private:
  common::Duration window_;
  std::deque<common::Nanos> timestamps_;
};

}  // namespace aegis::participant::stats
