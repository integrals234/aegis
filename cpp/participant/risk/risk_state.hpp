#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "cpp/common/time.hpp"
#include "cpp/participant/risk/risk_types.hpp"

/// Mutable engine-owned bookkeeping (ADR-0027): positions and reservations
/// risk tracks on its own, fed by POD fill/terminal notifications from the
/// composition root, never by depending on `portfolio::Portfolio` directly
/// (`cpp-participant-risk.may_depend_on` has no portfolio edge).
namespace aegis::participant::risk {

struct Reservation {
  std::uint32_t instrument_id{0};
  std::int64_t signed_quantity_units{0};  ///< Positive = reserved buy, negative = reserved sell.
  std::string strategy_id;
};

class RiskState {
 public:
  // ---- confirmed position (risk's own view, fed by on_fill) -------------
  void apply_fill(std::uint32_t instrument_id, Side side, std::int64_t fill_quantity_units) {
    position_by_instrument_[instrument_id] +=
        side == Side::kBuy ? fill_quantity_units : -fill_quantity_units;
  }
  [[nodiscard]] std::int64_t position_units(std::uint32_t instrument_id) const {
    const auto found = position_by_instrument_.find(instrument_id);
    return found == position_by_instrument_.end() ? 0 : found->second;
  }
  [[nodiscard]] const std::unordered_map<std::uint32_t, std::int64_t>& all_positions() const {
    return position_by_instrument_;
  }

  // ---- reservations (risk-approved, not yet terminal) --------------------
  void reserve(std::uint64_t client_order_id, std::uint32_t instrument_id, Side side,
               std::int64_t quantity_units, std::string strategy_id) {
    const std::int64_t signed_quantity = side == Side::kBuy ? quantity_units : -quantity_units;
    reservations_[client_order_id] = Reservation{.instrument_id = instrument_id,
                                                 .signed_quantity_units = signed_quantity,
                                                 .strategy_id = std::move(strategy_id)};
    reserved_by_instrument_[instrument_id] += signed_quantity;
  }
  /// A fill converts part of a reservation into confirmed position (the
  /// caller applies the fill via `apply_fill` separately); this only shrinks
  /// the outstanding reserved amount so the same exposure is never counted
  /// twice.
  void reduce_reservation(std::uint64_t client_order_id, std::int64_t fill_quantity_units) {
    const auto found = reservations_.find(client_order_id);
    if (found == reservations_.end()) {
      return;
    }
    const std::int64_t sign = found->second.signed_quantity_units >= 0 ? 1 : -1;
    const std::int64_t delta = sign * fill_quantity_units;
    found->second.signed_quantity_units -= delta;
    reserved_by_instrument_[found->second.instrument_id] -= delta;
  }
  /// Full release: cancel, reject, terminal fill, or (were the OMS to expose
  /// it) a failed adapter submission -- any path by which this order will
  /// never consume more exposure than it already has.
  void release_reservation(std::uint64_t client_order_id) {
    const auto found = reservations_.find(client_order_id);
    if (found == reservations_.end()) {
      return;
    }
    reserved_by_instrument_[found->second.instrument_id] -= found->second.signed_quantity_units;
    reservations_.erase(found);
  }
  [[nodiscard]] bool has_reservation(std::uint64_t client_order_id) const {
    return reservations_.contains(client_order_id);
  }
  [[nodiscard]] std::int64_t reserved_units(std::uint32_t instrument_id) const {
    const auto found = reserved_by_instrument_.find(instrument_id);
    return found == reserved_by_instrument_.end() ? 0 : found->second;
  }
  [[nodiscard]] const std::unordered_map<std::uint32_t, std::int64_t>& all_reservations() const {
    return reserved_by_instrument_;
  }
  [[nodiscard]] std::size_t reservation_count() const { return reservations_.size(); }

  // ---- idempotency ---------------------------------------------------
  [[nodiscard]] bool seen_before(const std::string& key) const {
    return dedupe_keys_.contains(key);
  }
  void mark_seen(const std::string& key) { dedupe_keys_.insert(key); }

  // ---- kill switches ---------------------------------------------------
  /// Returns true the first time a given strategy is tripped (a caller uses
  /// this to emit the trip event exactly once); a repeated trip is a no-op
  /// that returns false.
  bool trip_strategy(const std::string& strategy_id) {
    return halted_strategies_.insert(strategy_id).second;
  }
  bool trip_global() {
    if (global_halted_) {
      return false;
    }
    global_halted_ = true;
    return true;
  }
  [[nodiscard]] bool is_strategy_halted(const std::string& strategy_id) const {
    return halted_strategies_.contains(strategy_id);
  }
  [[nodiscard]] bool is_globally_halted() const { return global_halted_; }

  // ---- daily loss / drawdown latches -------------------------------------
  /// Both return true only the first time they trip, matching the kill
  /// switches' "emit once" contract (AEGIS-131's "triggers exactly once").
  bool trip_daily_loss() {
    if (daily_loss_tripped_) {
      return false;
    }
    daily_loss_tripped_ = true;
    return true;
  }
  bool trip_drawdown() {
    if (drawdown_tripped_) {
      return false;
    }
    drawdown_tripped_ = true;
    return true;
  }
  [[nodiscard]] bool is_daily_loss_tripped() const { return daily_loss_tripped_; }
  [[nodiscard]] bool is_drawdown_tripped() const { return drawdown_tripped_; }
  /// A new trading session (AEGIS-131) resets the daily-loss latch only --
  /// drawdown is a high-water-mark quantity that is never session-scoped.
  void start_new_session() { daily_loss_tripped_ = false; }

  // ---- market state -------------------------------------------------
  void note_market_quote(const MarketQuote& quote) {
    if (quote.valid) {
      last_valid_quote_[quote.instrument_id] = quote;
    }
    last_observed_at_nanos_[quote.instrument_id] = quote.observed_at_nanos;
  }
  [[nodiscard]] bool has_valid_quote(std::uint32_t instrument_id) const {
    return last_valid_quote_.contains(instrument_id);
  }
  [[nodiscard]] const MarketQuote* valid_quote(std::uint32_t instrument_id) const {
    const auto found = last_valid_quote_.find(instrument_id);
    return found == last_valid_quote_.end() ? nullptr : &found->second;
  }

  // ---- connectivity -------------------------------------------------
  void set_feed_connected(bool connected) { feed_connected_ = connected; }
  void set_exchange_connected(bool connected) { exchange_connected_ = connected; }
  void set_broker_connected(bool connected) { broker_connected_ = connected; }
  [[nodiscard]] bool feed_connected() const { return feed_connected_; }
  [[nodiscard]] bool exchange_connected() const { return exchange_connected_; }
  [[nodiscard]] bool broker_connected() const { return broker_connected_; }

  // ---- message rate limiting (AEGIS-128) ---------------------------
  // A plain sorted deque, not stats::RollingRate: rate limiting needs a
  // read-only "what if" count for the pure evaluate() path, which
  // RollingRate's mutate-only record_event() cannot provide.
  [[nodiscard]] std::size_t order_count_in_window(common::Nanos now,
                                                  common::Duration window) const {
    return count_in_window(order_timestamps_, now, window);
  }
  void record_order_event(common::Nanos now) { order_timestamps_.push_back(now); }
  [[nodiscard]] std::size_t cancel_count_in_window(common::Nanos now,
                                                   common::Duration window) const {
    return count_in_window(cancel_timestamps_, now, window);
  }
  void record_cancel_event(common::Nanos now) { cancel_timestamps_.push_back(now); }

  // ---- equity, fed by the composition root from Portfolio state ---------
  void set_equity_units(std::int64_t equity_units) { equity_units_ = equity_units; }
  [[nodiscard]] std::int64_t equity_units() const { return equity_units_; }

 private:
  std::unordered_map<std::uint32_t, std::int64_t> position_by_instrument_;
  std::unordered_map<std::uint64_t, Reservation> reservations_;
  std::unordered_map<std::uint32_t, std::int64_t> reserved_by_instrument_;
  std::unordered_set<std::string> dedupe_keys_;
  std::unordered_set<std::string> halted_strategies_;
  bool global_halted_{false};
  bool daily_loss_tripped_{false};
  bool drawdown_tripped_{false};
  std::unordered_map<std::uint32_t, MarketQuote> last_valid_quote_;
  std::unordered_map<std::uint32_t, common::Nanos> last_observed_at_nanos_;
  bool feed_connected_{true};
  bool exchange_connected_{true};
  bool broker_connected_{true};
  std::int64_t equity_units_{0};
  std::deque<common::Nanos> order_timestamps_;
  std::deque<common::Nanos> cancel_timestamps_;

  [[nodiscard]] static std::size_t count_in_window(const std::deque<common::Nanos>& timestamps,
                                                   common::Nanos now, common::Duration window) {
    const common::Nanos cutoff = now - window.nanos();
    std::size_t count = 0;
    for (auto it = timestamps.rbegin(); it != timestamps.rend() && *it > cutoff; ++it) {
      ++count;
    }
    return count;
  }
};

}  // namespace aegis::participant::risk
