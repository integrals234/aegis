#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "cpp/events/envelope.hpp"
#include "cpp/participant/oms/execution_adapter.hpp"

/// The second M3 production adapter (AEGIS-112, AEGIS-119; ADR-0023).
///
/// Drives the OMS through a committed, deterministic response script —
/// race orderings and rejection shapes no live matching engine can be made
/// to reproduce on demand. **Has no transport, no exchange, no paper/live
/// connectivity**: `submit`/`cancel`/`modify` only record that a call was
/// made and consume the next scripted response, exactly as written; nothing
/// here interprets, matches, or validates a command against book state, so
/// its output must never stand in for evidence of real matching behaviour
/// (that is `TransportExecutionAdapter` plus the real-exchange integration
/// harness in `tests/`).
namespace aegis::participant::oms {

/// One scripted inbound message: the exact wire form a real response would
/// arrive as, so a consumer decodes it identically regardless of which
/// adapter produced it (AEGIS-119's environment-independence).
struct ScriptedResponse {
  events::MessageType message_type{events::MessageType::kUnspecified};
  std::vector<std::byte> payload;
};

class RecordedResponseAdapter final : public ExecutionAdapter {
 public:
  /// `script` is consumed in order: the first `submit`/`cancel`/`modify`
  /// call (in any combination) delivers `script[0]`, the second delivers
  /// `script[1]`, and so on. A script shorter than the number of calls made
  /// leaves later calls with no response to deliver — a deliberate way to
  /// script "the exchange never responds to this one."
  explicit RecordedResponseAdapter(std::vector<ScriptedResponse> script)
      : script_(std::move(script)) {}

  /// Every call returns true (the "send" always succeeds — there is no
  /// transport to refuse it) and advances the script cursor by one,
  /// regardless of the command's own content: this adapter does not
  /// interpret what it was asked to do, only what it was told to answer.
  [[nodiscard]] bool submit(const events::exchange::NewOrderCommand& command) override;
  [[nodiscard]] bool cancel(const events::exchange::CancelOrderCommand& command) override;
  [[nodiscard]] bool modify(const events::exchange::ModifyOrderCommand& command) override;

  /// Pops and returns the next undelivered response that a `submit`/
  /// `cancel`/`modify` call has already advanced the script past, if any —
  /// each call to this method delivers exactly one response and then
  /// advances, so repeated calls drain the script in order rather than
  /// repeating the same entry. A caller (the OMS, or a test) drives this
  /// deterministically on its own schedule — nothing here delivers a
  /// response asynchronously or out of script order.
  [[nodiscard]] std::optional<ScriptedResponse> next_response();

  [[nodiscard]] std::size_t calls_made() const { return cursor_; }

 private:
  std::vector<ScriptedResponse> script_;
  std::size_t cursor_{0};     ///< Number of submit/cancel/modify calls made.
  std::size_t delivered_{0};  ///< Number of responses next_response() has returned.
};

}  // namespace aegis::participant::oms
