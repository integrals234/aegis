#pragma once

#include <cstdint>

/// The OMS order-lifecycle state machine (AEGIS-108; ADR-0023).
namespace aegis::participant::oms {

/// The ten frozen states. Every path from `kCreated` to `kSubmitted` passes
/// through `kRiskPending` -- there is no legal transition that skips it, so
/// the mandatory risk gate (`RiskGate`) is structural, not a convention a
/// caller could forget to honour.
///
/// NOLINTNEXTLINE(performance-enum-size)
enum class OrderState : std::uint8_t {
  kCreated = 0,
  kRiskPending = 1,
  kRejected = 2,
  kSubmitted = 3,
  kAcknowledged = 4,
  kPartiallyFilled = 5,
  kFilled = 6,
  kCancelPending = 7,
  kCancelled = 8,
  kExpired = 9,
};

/// `kRejected`, `kFilled`, `kCancelled`, `kExpired`: no legal transition
/// leads out of any of them.
[[nodiscard]] bool is_terminal(OrderState state);

/// The full transition table (AEGIS-108's frozen acceptance: "invalid
/// transitions are rejected"). `kPartiallyFilled -> kPartiallyFilled` is
/// deliberately legal -- a second partial fill on an already-partially-filled
/// order is not a state change. `kCancelPending -> kFilled` and
/// `kCancelPending -> kPartiallyFilled` are deliberately legal -- a fill can
/// race an in-flight cancel and arrive first (AEGIS-112).
[[nodiscard]] bool is_legal_transition(OrderState from, OrderState to);

}  // namespace aegis::participant::oms
