#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "cpp/exchange/order_book/book.hpp"
#include "cpp/exchange/order_book/instrument.hpp"
#include "cpp/exchange/sequencer/sequencer.hpp"
#include "cpp/exchange/state/event_log.hpp"

/// The composition root (ADR-0012): nothing else may legally see sequencer,
/// book, matching and state at once. `cpp-exchange-app` may depend on all of
/// them (`configs/architecture_rules.yaml`).
///
/// `ExchangeNode` registers instruments and owns one `OrderBook` per
/// instrument, plus the single `Sequencer` and `EventLog` shared across all of
/// them. Command processing (`MatchingEngine::apply`) is wired in as it lands.
namespace aegis::exchange {

class ExchangeNode {
 public:
  /// Registers a fresh, empty `OrderBook` for `spec.instrument_id`.
  /// Precondition: no book is already registered for that id.
  void add_instrument(const InstrumentSpec& spec, std::size_t order_capacity = 0);

  [[nodiscard]] const InstrumentSpec* instrument(InstrumentId id) const;
  [[nodiscard]] OrderBook* book(InstrumentId id);
  [[nodiscard]] const OrderBook* book(InstrumentId id) const;

  [[nodiscard]] Sequencer& sequencer() { return sequencer_; }
  [[nodiscard]] const Sequencer& sequencer() const { return sequencer_; }
  [[nodiscard]] EventLog& event_log() { return event_log_; }
  [[nodiscard]] const EventLog& event_log() const { return event_log_; }

 private:
  Sequencer sequencer_;
  EventLog event_log_;
  std::unordered_map<std::uint32_t, InstrumentSpec> instruments_;
  std::unordered_map<std::uint32_t, OrderBook> books_;
};

}  // namespace aegis::exchange
