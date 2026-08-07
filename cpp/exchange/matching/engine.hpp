#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cpp/events/exchange_messages.hpp"
#include "cpp/exchange/matching/matching_policy.hpp"
#include "cpp/exchange/order_book/book.hpp"
#include "cpp/exchange/order_book/instrument.hpp"

/// `MatchingEngine` applies one sequenced command to one book and *emits the
/// event vector* (ADR-0011) — it does not own a log or an `EventSequence`
/// counter itself, because `cpp-exchange-matching` may depend only on
/// `cpp-common`, `cpp-events` and `cpp-exchange-order-book`
/// (`configs/architecture_rules.yaml`), never on `cpp-exchange-state`. The
/// composition root (`cpp-exchange-app`) frames each emitted event into the
/// canonical `EventLog`.
namespace aegis::exchange {

/// One event the engine decided to emit, already wire-encoded
/// (`cpp/events/exchange_messages.hpp`). The caller frames it in an
/// `Envelope` with the `EventSequence` the log assigns.
struct EmittedEvent {
  events::MessageType message_type{events::MessageType::kUnspecified};
  std::vector<std::byte> payload;
};

class MatchingEngine {
 public:
  explicit MatchingEngine(const MatchingPolicy& policy) : policy_(&policy) {}

  /// Validates and applies `command` to `book` under `spec`'s grid, assigning
  /// a new `OrderId` on acceptance. `command_sequence` is the causal
  /// reference stamped into every emitted event's `causing_command_sequence`
  /// and, on acceptance, becomes the accepted order's `Priority`
  /// (`Priority::from(command_sequence)`) — the FIFO key AEGIS-027 requires.
  ///
  /// A market order (`command.order_type == kMarket`) whose quantity is not
  /// fully consumed by matching never rests: the residual is terminated with
  /// `TerminationReason::kResidualCanceled` (AEGIS-029; exercised by slice 3's
  /// tests, though the policy is implemented here since a limit order's
  /// residual-rests / market order's residual-terminates branch is one
  /// decision, not two).
  [[nodiscard]] std::vector<EmittedEvent> apply_new_order(
      OrderBook& book, const InstrumentSpec& spec, const events::exchange::NewOrderCommand& command,
      events::CommandSequence command_sequence);

  /// Cancels a live order the caller owns. `kUnknownOrderId` covers an
  /// order that never existed, already terminated, or was replaced —
  /// there is deliberately no separate "already terminal" reason
  /// (ADR-0011: no unbounded tombstone set). `kNotOrderOwner` covers a live
  /// order owned by a different participant.
  [[nodiscard]] static std::vector<EmittedEvent> apply_cancel_order(
      OrderBook& book, const events::exchange::CancelOrderCommand& command,
      events::CommandSequence command_sequence);

  /// Applies a decrease-in-place or a cancel-replace, per ADR-0011.
  /// `command.new_quantity_units` is the order's new *total* quantity (FIX
  /// `OrderQty` convention), not its new remaining — that is what makes
  /// `kModifyBelowFilled` a checkable condition. A price change or a
  /// quantity increase replaces the order with a new `OrderId` and a new
  /// `Priority` at the tail of its level, and the replacement is matched
  /// exactly like a fresh limit `NewOrder` (it can trade immediately if the
  /// new price crosses).
  [[nodiscard]] std::vector<EmittedEvent> apply_modify_order(
      OrderBook& book, const InstrumentSpec& spec,
      const events::exchange::ModifyOrderCommand& command,
      events::CommandSequence command_sequence);

 private:
  const MatchingPolicy* policy_;
  std::uint64_t next_order_id_{1};
};

}  // namespace aegis::exchange
