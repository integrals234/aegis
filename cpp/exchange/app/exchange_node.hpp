#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "cpp/exchange/matching/engine.hpp"
#include "cpp/exchange/matching/fifo_policy.hpp"
#include "cpp/exchange/order_book/book.hpp"
#include "cpp/exchange/order_book/instrument.hpp"
#include "cpp/exchange/sequencer/sequencer.hpp"
#include "cpp/exchange/state/event_log.hpp"

/// The composition root (ADR-0012): nothing else may legally see sequencer,
/// book, matching and state at once. `cpp-exchange-app` may depend on all of
/// them (`configs/architecture_rules.yaml`).
///
/// `ExchangeNode` registers instruments and owns one `OrderBook` per
/// instrument, plus the single `Sequencer`, `EventLog` and `MatchingEngine`
/// shared across all of them. It is the only layer that resolves
/// `instrument_id` to a book at all — `MatchingEngine` never sees an unknown
/// instrument, because there is no book to hand it one for. A command naming
/// an unregistered instrument is rejected here, before `MatchingEngine` is
/// ever called (`RejectReason::kUnknownInstrument`, AEGIS-035).
namespace aegis::exchange {

class ExchangeNode {
 public:
  ExchangeNode() : matching_engine_(policy_) {}

  /// Registers a fresh, empty `OrderBook` for `spec.instrument_id`.
  /// Precondition: no book is already registered for that id.
  void add_instrument(const InstrumentSpec& spec, std::size_t order_capacity = 0);

  [[nodiscard]] const InstrumentSpec* instrument(InstrumentId id) const;
  [[nodiscard]] OrderBook* book(InstrumentId id);
  [[nodiscard]] const OrderBook* book(InstrumentId id) const;

  /// Routes `command` to its instrument's book and `MatchingEngine`, or
  /// rejects it with `kUnknownInstrument` if no such book is registered.
  [[nodiscard]] std::vector<EmittedEvent> apply_new_order(
      const events::exchange::NewOrderCommand& command, events::CommandSequence command_sequence);
  [[nodiscard]] std::vector<EmittedEvent> apply_cancel_order(
      const events::exchange::CancelOrderCommand& command,
      events::CommandSequence command_sequence);
  [[nodiscard]] std::vector<EmittedEvent> apply_modify_order(
      const events::exchange::ModifyOrderCommand& command,
      events::CommandSequence command_sequence);

  [[nodiscard]] Sequencer& sequencer() { return sequencer_; }
  [[nodiscard]] const Sequencer& sequencer() const { return sequencer_; }
  [[nodiscard]] EventLog& event_log() { return event_log_; }
  [[nodiscard]] const EventLog& event_log() const { return event_log_; }

 private:
  Sequencer sequencer_;
  EventLog event_log_;
  FifoPolicy policy_;
  MatchingEngine matching_engine_;
  std::unordered_map<std::uint32_t, InstrumentSpec> instruments_;
  std::unordered_map<std::uint32_t, OrderBook> books_;
};

}  // namespace aegis::exchange
