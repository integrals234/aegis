#pragma once

#include "cpp/common/time.hpp"
#include "cpp/replay/replay_event.hpp"

/// The four approved replay pacing modes (M2 slice 10, AEGIS-054..057).
///
/// Every mode computes a **virtual** wait duration; none of them sleeps.
/// That is deliberate, not an oversight: a deterministic test path must stay
/// free of real sleeping or scheduler dependence, and a future caller (a
/// CLI, not built in M2) decides whether to act on the returned duration.
/// Because pacing only changes the computed wait between two already-fixed
/// events, all four modes emit the identical canonical event sequence for
/// the same input -- pacing never reorders or filters anything.
namespace aegis::replay {

class PacingPolicy {
 public:
  PacingPolicy() = default;
  PacingPolicy(const PacingPolicy&) = delete;
  PacingPolicy& operator=(const PacingPolicy&) = delete;
  PacingPolicy(PacingPolicy&&) = delete;
  PacingPolicy& operator=(PacingPolicy&&) = delete;
  virtual ~PacingPolicy() = default;

  /// The virtual wait before `current` would become due, given `previous`
  /// was just emitted. Never sleeps; purely computes a duration.
  [[nodiscard]] virtual common::Duration wait_before(const ReplayEvent& previous,
                                                     const ReplayEvent& current) const = 0;
};

/// AEGIS-054: replay at original relative timing -- the wait is exactly the
/// gap between the two events' own timestamps.
class OriginalSpeedPacing final : public PacingPolicy {
 public:
  [[nodiscard]] common::Duration wait_before(const ReplayEvent& previous,
                                             const ReplayEvent& current) const override;
};

/// AEGIS-055: replay at a configurable speed multiplier -- the original gap
/// divided by `multiplier`. `multiplier` must be strictly positive: zero or
/// negative has no meaningful "speed" interpretation and is refused at
/// construction rather than producing a nonsensical wait later.
class AcceleratedPacing final : public PacingPolicy {
 public:
  explicit AcceleratedPacing(double multiplier);

  [[nodiscard]] common::Duration wait_before(const ReplayEvent& previous,
                                             const ReplayEvent& current) const override;

 private:
  double multiplier_;
};

/// AEGIS-056: replay at a configurable fixed event rate, independent of the
/// original gaps -- every wait is `interval`, regardless of `previous`/
/// `current`. `interval` must be non-negative.
class FixedRatePacing final : public PacingPolicy {
 public:
  explicit FixedRatePacing(common::Duration interval);

  [[nodiscard]] common::Duration wait_before(const ReplayEvent& previous,
                                             const ReplayEvent& current) const override;

 private:
  common::Duration interval_;
};

/// AEGIS-057: advance one event (or, via `ReplayEngine::next_group`, one
/// timestamp group) at a time -- there is no automatic timing at all, so
/// the computed wait is always zero; advancement is entirely caller-driven.
class StepPacing final : public PacingPolicy {
 public:
  [[nodiscard]] common::Duration wait_before(const ReplayEvent& previous,
                                             const ReplayEvent& current) const override;
};

}  // namespace aegis::replay
