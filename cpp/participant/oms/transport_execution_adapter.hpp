#pragma once

#include <cstdint>
#include <vector>

#include "cpp/common/clock.hpp"
#include "cpp/events/envelope.hpp"
#include "cpp/participant/oms/execution_adapter.hpp"
#include "cpp/participant/oms/execution_transport.hpp"

/// The environment-independent execution path (AEGIS-119; ADR-0023).
namespace aegis::participant::oms {

/// Encodes OMS intent to `Envelope`s and hands them to an injected
/// `ExecutionTransport&`. Owns no responses of its own -- they arrive
/// asynchronously through whatever transport is injected, which is the seam
/// M9's paper transport eventually fills with a different concrete
/// transport, unchanged.
class TransportExecutionAdapter final : public ExecutionAdapter {
 public:
  TransportExecutionAdapter(ExecutionTransport& transport, common::WallClock& clock,
                            std::uint64_t stream_id)
      : transport_(&transport), clock_(&clock), stream_id_(stream_id) {}

  [[nodiscard]] bool submit(const events::exchange::NewOrderCommand& command) override;
  [[nodiscard]] bool cancel(const events::exchange::CancelOrderCommand& command) override;
  [[nodiscard]] bool modify(const events::exchange::ModifyOrderCommand& command) override;

 private:
  [[nodiscard]] events::Envelope frame(events::MessageType type, std::vector<std::byte> payload);

  ExecutionTransport* transport_;
  common::WallClock* clock_;
  std::uint64_t stream_id_;
  std::uint64_t next_sequence_{1};
};

}  // namespace aegis::participant::oms
