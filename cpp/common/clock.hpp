#pragma once

#include "cpp/common/time.hpp"

/// Clock interfaces (ADR-0002).
///
/// Every component that needs the time takes a clock rather than calling the
/// system clock directly. The reason is AEGIS-005: replay must produce
/// byte-identical canonical output from identical input, and a component that
/// reads the system clock produces different output on every run. Injection also
/// makes the M2 virtual clock a substitution rather than a rewrite.
///
/// There is no process-global clock and no `default_clock()` accessor. A global
/// would be reachable from every book partition, which is exactly the hidden
/// shared state the single-writer rule (AEGIS-047) exists to prevent, and its
/// absence has to be structural rather than a convention.
namespace aegis::common {

/// Source of UTC wall-clock readings.
///
/// Returns raw nanoseconds; the caller states which domain the reading belongs
/// to via `stamp<Domain>()`, so the domain is chosen deliberately at each site
/// rather than baked into the clock.
class WallClock {
 public:
  WallClock() = default;
  WallClock(const WallClock&) = delete;
  WallClock& operator=(const WallClock&) = delete;
  WallClock(WallClock&&) = delete;
  WallClock& operator=(WallClock&&) = delete;
  virtual ~WallClock() = default;

  /// Nanoseconds since the Unix epoch, UTC.
  [[nodiscard]] virtual Nanos now_utc() const = 0;

  /// Take a reading into a named clock domain.
  template <SerializableTimestamp T>
  [[nodiscard]] T stamp() const {
    return T{now_utc()};
  }
};

/// Source of monotonic readings, for latency measurement only.
class SteadyClock {
 public:
  SteadyClock() = default;
  SteadyClock(const SteadyClock&) = delete;
  SteadyClock& operator=(const SteadyClock&) = delete;
  SteadyClock(SteadyClock&&) = delete;
  SteadyClock& operator=(SteadyClock&&) = delete;
  virtual ~SteadyClock() = default;

  [[nodiscard]] virtual MonotonicTime now() const = 0;
};

/// Wall clock backed by the operating system.
class SystemWallClock final : public WallClock {
 public:
  [[nodiscard]] Nanos now_utc() const override;
};

/// Monotonic clock backed by the operating system.
class SystemSteadyClock final : public SteadyClock {
 public:
  [[nodiscard]] MonotonicTime now() const override;
};

/// Wall clock a test drives by hand.
///
/// The point of a manual clock is not convenience: a fixture whose timestamps
/// come from the system clock cannot assert on a byte-for-byte log record, so
/// the determinism harness would have nothing stable to hash.
class ManualClock final : public WallClock {
 public:
  explicit ManualClock(Nanos start = 0) : now_(start) {}

  [[nodiscard]] Nanos now_utc() const override { return now_; }

  void set(Nanos nanos) { now_ = nanos; }
  void advance(Duration delta) { now_ += delta.nanos(); }

 private:
  Nanos now_;
};

/// Monotonic clock a test drives by hand.
///
/// Advancing backwards is impossible by construction: a monotonic clock that can
/// move backwards would let a latency measurement come out negative, and the
/// component under test would have no way to distinguish that from a real bug.
class ManualSteadyClock final : public SteadyClock {
 public:
  explicit ManualSteadyClock(Nanos start = 0) : now_(start) {}

  [[nodiscard]] MonotonicTime now() const override { return MonotonicTime{now_}; }

  /// Advance by a non-negative duration. A negative delta is clamped to zero.
  void advance(Duration delta) { now_ += delta.nanos() > 0 ? delta.nanos() : 0; }

 private:
  Nanos now_;
};

}  // namespace aegis::common
