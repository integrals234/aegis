#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "cpp/events/exchange_messages.hpp"
#include "cpp/participant/oms/cost_model.hpp"
#include "cpp/participant/oms/execution_adapter.hpp"
#include "cpp/participant/oms/latency_model.hpp"
#include "cpp/participant/oms/missed_trade_tracker.hpp"
#include "cpp/participant/oms/order_lifecycle.hpp"
#include "cpp/participant/oms/risk_gate.hpp"

/// Ties the lifecycle state machine, the mandatory risk seam and an
/// `ExecutionAdapter` to per-order tracked state (AEGIS-108, AEGIS-109,
/// AEGIS-110, AEGIS-111, AEGIS-112, AEGIS-114, AEGIS-119; ADR-0023).
///
/// `OrderManager` does not itself decide whether an order fills, partially
/// fills, or rests — that is real (or scripted) matching behaviour arriving
/// through `handle_*`, decoded by the caller from whatever
/// `ExecutionAdapter`/`ExecutionTransport` is injected. This class only
/// keeps the state machine honest against what actually arrives: it never
/// mutates state on its own guess about what should have happened.
namespace aegis::participant::oms {

struct TrackedOrder {
  OrderLifecycle lifecycle;
  std::uint64_t client_order_id{0};
  std::uint64_t exchange_order_id{0};  ///< 0 until `handle_order_accepted`.
  std::uint32_t instrument_id{0};
  std::uint64_t participant_id{0};
  events::exchange::Side side{events::exchange::Side::kBuy};
  std::int64_t price_units{0};  ///< The order's own price; 0 and meaningless for a market order.
  std::int64_t original_quantity_units{0};
  std::int64_t cumulative_filled_units{0};  ///< AEGIS-114: sum of every fill applied so far.
  std::int64_t remaining_units{0};

  /// AEGIS-116, accumulated as fills land rather than recomputed: total fees
  /// charged on this order, and total signed slippage cost measured against
  /// the order's own price as the reference (positive is adverse). Both are
  /// zero for an order that never filled.
  std::int64_t cumulative_fees_units{0};
  std::int64_t cumulative_slippage_cost_units{0};

  /// AEGIS-113: the five-stage latency attribution for the market event that
  /// motivated this order. Present only when the manager was given a
  /// `LatencyModel` and the caller supplied the event's own `EventTime`.
  /// Deliberately last, and deliberately NOT part of `OmsSnapshot`: it is
  /// derived from a market event, not persisted state, and a restored order
  /// has no event to attribute.
  std::optional<LatencyAttribution> latency;

  friend bool operator==(const TrackedOrder&, const TrackedOrder&) = default;
};

class OrderManager {
 public:
  /// Neither reference is owned; both must outlive this object — the same
  /// injection discipline as `TransportExecutionAdapter`.
  OrderManager(ExecutionAdapter& adapter, RiskGate& risk_gate, FeeSchedule fees = {},
               std::optional<LatencyConfig> latency = std::nullopt)
      : adapter_(&adapter),
        risk_gate_(&risk_gate),
        fees_(fees),
        latency_(latency.has_value() ? std::optional<LatencyModel>{LatencyModel{*latency}}
                                     : std::nullopt) {}

  /// Restoring constructor (AEGIS-237; ADR-0024): rebuilds tracked-order
  /// state and the client/exchange id indexes directly from
  /// `restored_orders`, bypassing `submit_new_order`'s risk-seam and
  /// adapter-call side effects entirely -- those already happened in the run
  /// that produced the snapshot, and replaying them again would resubmit
  /// orders that already exist at the exchange or re-consult a risk gate
  /// about a decision it already made. This is the "dedicated trusted
  /// construction path" the recovery contract calls for: it does not
  /// re-validate the transition history that produced each order's
  /// `lifecycle` (the snapshot codec that supplied `restored_orders` is
  /// responsible for that), but it does rebuild the index invariants
  /// (`exchange_to_client_id_` populated for every order with a nonzero
  /// `exchange_order_id`) exactly as the ordinary handlers would have left
  /// them, so every subsequent `handle_*` call behaves identically to an
  /// uninterrupted run. `next_client_order_id` continues numbering exactly
  /// where the snapshotted run left off, so a client_order_id is never
  /// reused.
  OrderManager(ExecutionAdapter& adapter, RiskGate& risk_gate,
               const std::vector<TrackedOrder>& restored_orders, std::uint64_t next_client_order_id,
               FeeSchedule fees = {})
      : adapter_(&adapter),
        risk_gate_(&risk_gate),
        fees_(fees),
        next_client_order_id_(next_client_order_id) {
    for (const auto& order : restored_orders) {
      const std::uint64_t client_order_id = order.client_order_id;
      const std::uint64_t exchange_order_id = order.exchange_order_id;
      orders_by_client_id_.emplace(client_order_id, order);
      if (exchange_order_id != 0) {
        exchange_to_client_id_[exchange_order_id] = client_order_id;
      }
    }
  }

  /// The full mandatory path: `Created -> RiskPending ->` (`risk_gate_`'s
  /// verdict) `-> Submitted -> adapter_.submit()`, or `-> Rejected` with the
  /// adapter never called (AEGIS-108's structural risk seam). A `kResize`
  /// verdict submits `resized_quantity_units`, not the caller's own —
  /// `original_quantity_units` records what was actually sent. Returns the
  /// participant-assigned `client_order_id` this order is tracked under.
  [[nodiscard]] std::uint64_t submit_new_order(
      std::uint32_t instrument_id, std::uint64_t participant_id, events::exchange::Side side,
      events::exchange::OrderType order_type, std::int64_t price_units, std::int64_t quantity_units,
      common::EventTime market_event_time = {});

  /// Requires the order to have been acknowledged (has an
  /// `exchange_order_id`) and to be in a state `kCancelPending` may legally
  /// follow. Returns false without calling the adapter if either does not
  /// hold — a cancel this manager cannot yet address is a caller error, not
  /// something to send anyway.
  [[nodiscard]] bool cancel_order(std::uint64_t client_order_id);

  /// Same preconditions as `cancel_order`; does not itself change lifecycle
  /// state (the frozen state list has no separate "modify pending" state —
  /// see `order_state.hpp`), since acknowledgement/rejection arrives as an
  /// ordinary `OrderReplacedEvent`/`OrderModifiedEvent`/`OrderRejectedEvent`.
  [[nodiscard]] bool modify_order(std::uint64_t client_order_id, std::int64_t new_price_units,
                                  std::int64_t new_quantity_units);

  /// `Submitted -> Acknowledged` (AEGIS-109/110/111): records the exchange's
  /// assigned `order_id` and indexes it for the `handle_*` calls below,
  /// which arrive keyed by that id, not by `client_order_id`.
  void handle_order_accepted(const events::exchange::OrderAcceptedEvent& event);

  /// `RiskPending -> Rejected` for a rejected new order (keyed by
  /// `client_order_id`); `kCancelPending -> kAcknowledged` for a rejected
  /// cancel (keyed by `order_id`) — the order remains live.
  void handle_order_rejected(const events::exchange::OrderRejectedEvent& event);

  /// A cancel-replace's old-order half: mirrors `handle_order_terminated`
  /// for `old_order_id`, then re-indexes tracking under `new_order_id`
  /// (AEGIS-112).
  void handle_order_replaced(const events::exchange::OrderReplacedEvent& event);

  /// AEGIS-109/110/111/114: applies one fill to the tracked order on
  /// whichever side of `event` matches a tracked `order_id`
  /// (`maker_order_id` or `taker_order_id`) — a no-op for the side this
  /// manager is not tracking. Updates `cumulative_filled_units`/
  /// `remaining_units` and always transitions to `kPartiallyFilled`, even if
  /// `remaining_units` has just reached zero: the authoritative "fully
  /// filled" transition is never inferred from arithmetic, it waits for the
  /// exchange's own `OrderTerminatedEvent{kFilled}`
  /// (`handle_order_terminated`), matching the two-event shape the exchange
  /// actually emits (ADR-0011).
  void handle_trade(const events::exchange::TradeEvent& event);

  /// `-> kFilled`/`kCancelled` per `event.reason`; `kResidualCanceled` (a
  /// market order's unfilled tail, ADR-0011) is treated as `kCancelled`.
  void handle_order_terminated(const events::exchange::OrderTerminatedEvent& event);

  [[nodiscard]] const TrackedOrder* find_by_client_order_id(std::uint64_t client_order_id) const;
  [[nodiscard]] const TrackedOrder* find_by_exchange_order_id(
      std::uint64_t exchange_order_id) const;

  /// AEGIS-237; ADR-0024 capture support. Every tracked order, in ascending
  /// `client_order_id` order -- `orders_by_client_id_` is unordered, and a
  /// snapshot's bytes must not depend on hash-table iteration order
  /// (`docs/RECOVERY_CONTRACT.md` byte-stability obligation).
  [[nodiscard]] std::vector<TrackedOrder> all_tracked_orders() const;

  [[nodiscard]] std::uint64_t next_client_order_id() const { return next_client_order_id_; }

  /// AEGIS-117. Populated by `handle_order_rejected` and
  /// `handle_order_terminated` as orders end without trading their full size
  /// -- this manager is the only thing that knows an order's untraded
  /// remainder, so recording it anywhere else would mean duplicating its
  /// bookkeeping.
  [[nodiscard]] const MissedTradeTracker& missed_trades() const { return missed_trades_; }

  /// AEGIS-113: true when this manager was configured with a latency model,
  /// so every order it submits carries a five-stage attribution.
  [[nodiscard]] bool models_latency() const { return latency_.has_value(); }

  /// AEGIS-116, summed across every tracked order.
  [[nodiscard]] std::int64_t total_fees_units() const;
  [[nodiscard]] std::int64_t total_slippage_cost_units() const;

 private:
  [[nodiscard]] TrackedOrder* find_mutable_by_exchange_order_id(std::uint64_t exchange_order_id);
  void apply_fill_to(std::uint64_t exchange_order_id, std::int64_t fill_quantity_units,
                     std::int64_t fill_price_units);

  /// Records `tracked`'s untraded remainder, if any, exactly once.
  void record_missed_remainder(const TrackedOrder& tracked);

  ExecutionAdapter* adapter_;
  RiskGate* risk_gate_;
  FeeSchedule fees_{};
  std::optional<LatencyModel> latency_;
  MissedTradeTracker missed_trades_;
  std::uint64_t next_client_order_id_{1};
  std::unordered_map<std::uint64_t, TrackedOrder> orders_by_client_id_;
  std::unordered_map<std::uint64_t, std::uint64_t> exchange_to_client_id_;
};

}  // namespace aegis::participant::oms
