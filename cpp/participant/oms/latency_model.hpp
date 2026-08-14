#pragma once

#include "cpp/common/time.hpp"

/// Deterministic path-latency modelling (AEGIS-113; ADR-0023).
///
/// `cpp/common/time.hpp` defines `ReceiveTime`/`DecisionTime`/`SubmitTime`/
/// `AckTime` as distinct domain types specifically so a cross-domain
/// subtraction never compiles by accident (ADR-0002). This is the one place
/// that difference is bridged deliberately: `LatencyModel::attribute()`
/// carries a single `ReceiveTime` forward through three *committed,
/// deterministic* stage delays -- never sampled, never wall-clock derived,
/// matching the M2 `DeterministicFaultInjector` convention -- reconstructing
/// each later domain explicitly via `.nanos()` rather than through the
/// (deliberately absent) cross-domain `operator-`. The four resulting
/// timestamps all descend from the one `ReceiveTime` the caller supplies, so
/// they are never a mix of readings from two unrelated clock sources.
namespace aegis::participant::oms {

/// One stage-by-stage breakdown of the path from a participant's receipt of
/// a market observation through to the exchange's acknowledgement of the
/// order it decided to send.
struct LatencyAttribution {
  common::ReceiveTime receive_time;
  common::DecisionTime decision_time;
  common::SubmitTime submit_time;
  common::AckTime ack_time;

  [[nodiscard]] common::Duration decision_latency() const {
    return common::Duration{decision_time.nanos() - receive_time.nanos()};
  }
  [[nodiscard]] common::Duration submit_latency() const {
    return common::Duration{submit_time.nanos() - decision_time.nanos()};
  }
  [[nodiscard]] common::Duration ack_latency() const {
    return common::Duration{ack_time.nanos() - submit_time.nanos()};
  }
  [[nodiscard]] common::Duration total_latency() const {
    return common::Duration{ack_time.nanos() - receive_time.nanos()};
  }

  /// AEGIS-113's acceptance criterion, checkable directly: the three stage
  /// latencies account for the entire path with nothing left unattributed.
  [[nodiscard]] bool reconciles() const {
    return (decision_latency() + submit_latency() + ack_latency()) == total_latency();
  }
};

/// Committed, per-stage delay configuration. Every field is a fixed
/// simulated duration supplied by the caller (a scenario fixture or the app
/// layer's configuration) -- this model has no notion of "typical" or
/// "realistic" latency and makes no benchmark claim (`docs/BENCHMARK_POLICY.md`
/// does not apply: nothing here is measured).
struct LatencyConfig {
  common::Duration decision_delay{};  ///< ReceiveTime -> DecisionTime.
  common::Duration submit_delay{};    ///< DecisionTime -> SubmitTime.
  common::Duration ack_delay{};       ///< SubmitTime -> AckTime.
};

class LatencyModel {
 public:
  explicit LatencyModel(LatencyConfig config) : config_(config) {}

  /// Pure function of `receive_time` and the configured delays -- no clock
  /// is read here, so the same input always reproduces the same attribution
  /// (AEGIS-005).
  [[nodiscard]] LatencyAttribution attribute(common::ReceiveTime receive_time) const;

 private:
  LatencyConfig config_;
};

}  // namespace aegis::participant::oms
