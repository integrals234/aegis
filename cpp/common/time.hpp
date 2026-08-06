#pragma once

#include <compare>  // required for the defaulted three-way comparisons below
#include <cstdint>

/// Time domains for AEGIS (ADR-0002).
///
/// docs/ARCHITECTURE.md lists seven distinct clocks — event, receive, decision,
/// submit, exchange, acknowledgement/fill and monotonic — and forbids comparing
/// wall-clock stamps for latency without documenting synchronisation. All seven
/// are 64-bit integers, so nothing about their representation stops a subtraction
/// that mixes domains, and the result of such a subtraction is a plausible-looking
/// number rather than an error. That is how a latency figure computed across two
/// unrelated clocks reaches a benchmark report.
///
/// So the domains are separated in the type system instead: each is a distinct
/// type, and only same-domain arithmetic compiles. The compiler rejects
/// `ack_time - monotonic_time` the way it rejects adding a pointer to a string.
namespace aegis::common {

/// Nanoseconds. The unit for every AEGIS timestamp and duration.
///
/// Signed on purpose: durations are routinely negative (an event that arrives
/// before its predecessor's stamp, a clock stepping backwards), and unsigned
/// wrap-around would hide exactly the anomalies replay validation must detect.
using Nanos = std::int64_t;

/// A signed span of time, independent of any clock domain.
class Duration {
 public:
  constexpr Duration() = default;
  constexpr explicit Duration(Nanos nanos) : nanos_(nanos) {}

  [[nodiscard]] constexpr Nanos nanos() const { return nanos_; }
  [[nodiscard]] constexpr double micros() const { return static_cast<double>(nanos_) / 1e3; }
  [[nodiscard]] constexpr double millis() const { return static_cast<double>(nanos_) / 1e6; }
  [[nodiscard]] constexpr double seconds() const { return static_cast<double>(nanos_) / 1e9; }

  constexpr auto operator<=>(const Duration&) const = default;
  constexpr bool operator==(const Duration&) const = default;

  constexpr Duration operator+(Duration other) const { return Duration{nanos_ + other.nanos_}; }
  constexpr Duration operator-(Duration other) const { return Duration{nanos_ - other.nanos_}; }
  constexpr Duration operator-() const { return Duration{-nanos_}; }

 private:
  Nanos nanos_{0};
};

/// Tags naming the seven clock domains of docs/ARCHITECTURE.md.
///
/// They are incomplete types: they exist only to make two instantiations of
/// Timestamp unrelated, and nothing may ever construct one.
namespace domain {
struct Event;      ///< Source/exchange event timestamp.
struct Receive;    ///< Participant receipt timestamp.
struct Decision;   ///< Strategy or human action timestamp.
struct Submit;     ///< OMS/gateway submission timestamp.
struct Exchange;   ///< Exchange processing timestamp.
struct Ack;        ///< Acknowledgement/execution timestamp.
struct Monotonic;  ///< Local monotonic reading; latency measurement only.
}  // namespace domain

/// A point in time within exactly one clock domain.
///
/// Construction from raw nanoseconds is explicit, so crossing a domain boundary
/// is always visible at the call site rather than implicit in an assignment.
template <typename Domain>
class Timestamp {
 public:
  using domain_type = Domain;

  constexpr Timestamp() = default;
  constexpr explicit Timestamp(Nanos nanos) : nanos_(nanos) {}

  [[nodiscard]] constexpr Nanos nanos() const { return nanos_; }

  constexpr auto operator<=>(const Timestamp&) const = default;
  constexpr bool operator==(const Timestamp&) const = default;

  /// Same-domain difference. There is deliberately no cross-domain overload.
  constexpr Duration operator-(Timestamp other) const { return Duration{nanos_ - other.nanos_}; }

  constexpr Timestamp operator+(Duration delta) const { return Timestamp{nanos_ + delta.nanos()}; }
  constexpr Timestamp operator-(Duration delta) const { return Timestamp{nanos_ - delta.nanos()}; }

 private:
  Nanos nanos_{0};
};

using EventTime = Timestamp<domain::Event>;
using ReceiveTime = Timestamp<domain::Receive>;
using DecisionTime = Timestamp<domain::Decision>;
using SubmitTime = Timestamp<domain::Submit>;
using ExchangeTime = Timestamp<domain::Exchange>;
using AckTime = Timestamp<domain::Ack>;
using MonotonicTime = Timestamp<domain::Monotonic>;

/// True for every timestamp that may appear in a persisted or hashed record.
///
/// A monotonic reading is meaningful only within one process run: its origin is
/// unspecified and differs across processes and reboots. Serialising one would
/// produce a record that replays to a different value on the next run, which
/// breaks the byte-identical canonical output AEGIS-005 requires.
///
/// Opt-in rather than opt-out: a domain added later is not serializable until
/// somebody states that it is.
template <typename T>
inline constexpr bool kSerializableTimestamp = false;

template <>
inline constexpr bool kSerializableTimestamp<EventTime> = true;
template <>
inline constexpr bool kSerializableTimestamp<ReceiveTime> = true;
template <>
inline constexpr bool kSerializableTimestamp<DecisionTime> = true;
template <>
inline constexpr bool kSerializableTimestamp<SubmitTime> = true;
template <>
inline constexpr bool kSerializableTimestamp<ExchangeTime> = true;
template <>
inline constexpr bool kSerializableTimestamp<AckTime> = true;

/// Concept form, used to constrain encoders.
template <typename T>
concept SerializableTimestamp = kSerializableTimestamp<T>;

/// The only supported way to move a timestamp into a persisted record.
///
/// Constrained rather than documented: `serialize_nanos(monotonic)` fails to
/// compile, so the rule cannot be forgotten under deadline pressure.
template <SerializableTimestamp T>
[[nodiscard]] constexpr Nanos serialize_nanos(T stamp) {
  return stamp.nanos();
}

/// Latency between two monotonic readings taken in the same process.
///
/// The only sanctioned way to measure elapsed time. docs/ARCHITECTURE.md forbids
/// deriving latency from wall-clock stamps without documenting synchronisation,
/// and this signature makes the sanctioned path the convenient one.
[[nodiscard]] constexpr Duration elapsed(MonotonicTime start, MonotonicTime end) {
  return end - start;
}

inline constexpr Nanos kNanosPerMicro = 1'000;
inline constexpr Nanos kNanosPerMilli = 1'000'000;
inline constexpr Nanos kNanosPerSecond = 1'000'000'000;

[[nodiscard]] constexpr Duration micros(Nanos count) { return Duration{count * kNanosPerMicro}; }
[[nodiscard]] constexpr Duration millis(Nanos count) { return Duration{count * kNanosPerMilli}; }
[[nodiscard]] constexpr Duration seconds(Nanos count) { return Duration{count * kNanosPerSecond}; }

}  // namespace aegis::common
