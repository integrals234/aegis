#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <unordered_map>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/exchange/market_data/market_data_publisher.hpp"
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

  /// Restores position after a snapshot (ADR-0013): `sequencer_`,
  /// `event_log_` and `matching_engine_` all start from the values a prior
  /// run's `state::ExchangeSnapshot` recorded, rather than from 1. The
  /// caller still owns `add_instrument()` for every instrument the snapshot
  /// covers and replaying `state::restore_orders_into()` for each — a
  /// snapshot carries no `InstrumentSpec`, so there is no book to restore
  /// orders into until the caller supplies one (`cpp-exchange-state` may not
  /// depend on `cpp-exchange-app`, so it cannot do this itself).
  ExchangeNode(events::CommandSequence next_command_sequence,
               common::ExchangeTime last_exchange_time, events::EventSequence next_event_sequence,
               std::uint64_t next_order_id)
      : sequencer_(next_command_sequence, last_exchange_time),
        event_log_(next_event_sequence),
        matching_engine_(policy_, next_order_id) {}

  /// Registers a fresh, empty `OrderBook` for `spec.instrument_id`, backed by
  /// `resource` (ADR-0010; defaults to the global resource, so every
  /// existing call site is unaffected). `aegis_exchange_bench` is the one
  /// caller that passes its own, to measure per-instance allocation counts
  /// against a benchmark workload the way
  /// `tests/cpp/property/test_allocation_counters.cpp` measures them against
  /// a unit-test workload.
  /// Precondition: no book is already registered for that id.
  void add_instrument(const InstrumentSpec& spec, std::size_t order_capacity = 0,
                      std::pmr::memory_resource* resource = std::pmr::get_default_resource());

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
  [[nodiscard]] std::uint64_t next_order_id() const { return matching_engine_.next_order_id(); }

  /// The market-data half of the composition root (AEGIS-064, AEGIS-065;
  /// ADR-0020). `derive_market_data` converts an `apply_*` call's already-
  /// emitted events to the shape `cpp-exchange-market-data` accepts (that
  /// layer may not depend on `cpp-exchange-matching`, so it cannot name
  /// `EmittedEvent` itself) and forwards them; `capture_market_data_snapshot`
  /// reads `book(id)` read-only. Neither method changes what `apply_*`
  /// returns or how it matches — purely additive, observing after the fact.
  [[nodiscard]] std::vector<events::market_data::BookDeltaEvent> derive_market_data(
      const std::vector<EmittedEvent>& emitted);
  [[nodiscard]] events::market_data::BookSnapshotEvent capture_market_data_snapshot(
      InstrumentId id) const;

 private:
  Sequencer sequencer_;
  EventLog event_log_;
  FifoPolicy policy_;
  MatchingEngine matching_engine_;
  std::unordered_map<std::uint32_t, InstrumentSpec> instruments_;
  std::unordered_map<std::uint32_t, OrderBook> books_;
  MarketDataPublisher market_data_publisher_;
};

}  // namespace aegis::exchange
