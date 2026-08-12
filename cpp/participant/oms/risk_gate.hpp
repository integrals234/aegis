#pragma once

#include <cstdint>
#include <string>

#include "cpp/events/exchange_messages.hpp"

/// The mandatory risk seam (AEGIS-108; ADR-0023).
///
/// `RiskGate` is declared here, by the OMS, because every legal path from
/// `OrderState::kCreated` to `kSubmitted` passes through `kRiskPending`
/// (`order_state.hpp`) and something has to decide what happens there. **No
/// concrete implementation of this interface ships in production code at
/// M3** -- `cpp-participant-risk` (the layer that will implement real policy)
/// is dated M5 and stays empty until then. The only implementations that
/// exist before M5 live in test fixtures, explicitly named as test doubles,
/// so nothing in this library can be mistaken for validated risk logic.
namespace aegis::participant::oms {

/// NOLINTNEXTLINE(performance-enum-size)
enum class RiskVerdict : std::uint8_t {
  kApprove = 0,
  kResize = 1,
  kReject = 2,
};

struct RiskDecision {
  RiskVerdict verdict{RiskVerdict::kReject};
  /// Meaningful only when `verdict == kResize`: the quantity the gate will
  /// actually let through, in place of the command's own.
  std::int64_t resized_quantity_units{0};
  std::string reason;
};

class RiskGate {
 public:
  RiskGate() = default;
  RiskGate(const RiskGate&) = delete;
  RiskGate& operator=(const RiskGate&) = delete;
  RiskGate(RiskGate&&) = delete;
  RiskGate& operator=(RiskGate&&) = delete;
  virtual ~RiskGate() = default;

  [[nodiscard]] virtual RiskDecision decide(
      const events::exchange::NewOrderCommand& command) const = 0;
};

}  // namespace aegis::participant::oms
