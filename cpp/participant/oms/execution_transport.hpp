#pragma once

#include "cpp/events/envelope.hpp"

/// The wire transport boundary an `ExecutionAdapter` delegates to
/// (AEGIS-119; ADR-0023).
///
/// Carries encoded `Envelope`s only -- never an exchange type, never a
/// decoded command. This is the seam a concrete transport fills: an
/// in-process queue for a test harness, a real socket for M9's paper
/// adapter, a recorded-script reader for `RecordedResponseAdapter` (slice 5).
/// None of those concrete choices changes what `ExecutionAdapter` or the OMS
/// see.
namespace aegis::participant::oms {

class ExecutionTransport {
 public:
  ExecutionTransport() = default;
  ExecutionTransport(const ExecutionTransport&) = delete;
  ExecutionTransport& operator=(const ExecutionTransport&) = delete;
  ExecutionTransport(ExecutionTransport&&) = delete;
  ExecutionTransport& operator=(ExecutionTransport&&) = delete;
  virtual ~ExecutionTransport() = default;

  /// Returns false if the transport could not accept `envelope` right now
  /// (not connected, backpressure) -- an ordinary transport-level outcome,
  /// never an exception.
  [[nodiscard]] virtual bool send(const events::Envelope& envelope) = 0;
};

}  // namespace aegis::participant::oms
