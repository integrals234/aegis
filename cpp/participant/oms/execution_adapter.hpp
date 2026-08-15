#pragma once

#include "cpp/events/exchange_messages.hpp"

/// The outbound execution boundary (AEGIS-119; ADR-0023).
///
/// Defined **purely over `cpp/events` types** -- `submit`/`cancel`/`modify`
/// take the same `NewOrderCommand`/`CancelOrderCommand`/`ModifyOrderCommand`
/// AEGIS-004 already placed in `cpp/events` so a consumer can decode them
/// without depending on the exchange layer (ADR-0009). `ExecutionAdapter`
/// itself names no exchange type, which is what "environment-independent" in
/// AEGIS-119's title means concretely: an implementation can be satisfied by
/// something that has never heard of `ExchangeNode`.
namespace aegis::participant::oms {

class ExecutionAdapter {
 public:
  ExecutionAdapter() = default;
  ExecutionAdapter(const ExecutionAdapter&) = delete;
  ExecutionAdapter& operator=(const ExecutionAdapter&) = delete;
  ExecutionAdapter(ExecutionAdapter&&) = delete;
  ExecutionAdapter& operator=(ExecutionAdapter&&) = delete;
  virtual ~ExecutionAdapter() = default;

  /// Returns false if the command could not be sent (e.g. transport not
  /// connected) -- an ordinary outcome the OMS's own state machine reacts
  /// to, never an exception.
  [[nodiscard]] virtual bool submit(const events::exchange::NewOrderCommand& command) = 0;
  [[nodiscard]] virtual bool cancel(const events::exchange::CancelOrderCommand& command) = 0;
  [[nodiscard]] virtual bool modify(const events::exchange::ModifyOrderCommand& command) = 0;
};

}  // namespace aegis::participant::oms
