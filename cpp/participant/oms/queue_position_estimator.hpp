#pragma once

#include <cstdint>

/// Queue-position approximation (AEGIS-115; ADR-0023).
///
/// **This is explicitly an approximation, not exchange truth.** Order-level
/// queue position is only knowable exactly from a real matching engine's own
/// FIFO state (`cpp/exchange/order_book`); a participant reconstructing from
/// a public feed cannot see which resting orders ahead of its own have
/// already been cancelled, only the aggregate quantity a feed reports. This
/// class states plainly what it assumes and what it cannot know, rather
/// than presenting an estimate as if it were observed fact.
namespace aegis::participant::oms {

struct QueuePositionEstimate {
  /// The aggregate resting quantity a feed reported ahead of this order at
  /// the time it was submitted — ground truth for *this number*, but it is
  /// already an aggregate, not a per-order count (AEGIS-066 MBO sessions
  /// narrow this; AEGIS-067 MBP-only sessions cannot).
  std::int64_t observed_volume_ahead_units{0};

  /// Caller-supplied assumption: the fraction of `observed_volume_ahead_units`
  /// believed to cancel before this order would be reached. Stated as an
  /// assumption because it is not observable — this is the single largest
  /// source of error in the estimate, named explicitly rather than folded
  /// silently into the output number.
  double assumed_cancellation_rate{0.0};

  /// `observed_volume_ahead_units * (1 - assumed_cancellation_rate)`.
  std::int64_t effective_volume_ahead_units{0};

  /// `clamp(traded_volume_since_units / effective_volume_ahead_units, 0, 1)`
  /// — how much of the effective queue ahead has traded away since
  /// submission, as a heuristic proxy for "how close is this order to the
  /// front." `0.0` when there was nothing ahead to begin with (already at
  /// the front) is intentionally reported as `1.0`: an empty queue ahead
  /// means nothing stands between this order and a fill.
  double fill_probability{0.0};
};

class QueuePositionEstimator {
 public:
  QueuePositionEstimator() = delete;

  /// Precondition: `0.0 <= assumed_cancellation_rate <= 1.0`,
  /// `observed_volume_ahead_units >= 0`, `traded_volume_since_units >= 0`.
  [[nodiscard]] static QueuePositionEstimate estimate(std::int64_t observed_volume_ahead_units,
                                                      double assumed_cancellation_rate,
                                                      std::int64_t traded_volume_since_units);
};

}  // namespace aegis::participant::oms
