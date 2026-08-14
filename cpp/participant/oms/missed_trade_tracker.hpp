#pragma once

#include <cstdint>
#include <vector>

/// Missed-trade attribution (AEGIS-117; ADR-0023).
///
/// A "missed trade" here is purely arithmetic: the untraded remainder of an
/// order once it can no longer trade -- an outright exchange rejection (the
/// full requested quantity), or a terminated order's `original_quantity -
/// cumulative_filled` (a cancellation, a residual-cancel, a race lost to a
/// cancel). This tracker does not decide *why* a quantity went untraded and
/// does not model what a strategy might have captured instead -- doing so
/// would require the strategy layer (M4) or the risk layer (M5), neither of
/// which exists yet. It only accumulates the numbers `OrderManager`'s own
/// tracked state already carries, so it is not a new source of truth.
///
/// A caller-side risk rejection is deliberately never recorded here: the
/// participant's own risk gate declining to send an order is not a missed
/// *market* opportunity in the sense this requirement is about, it is a
/// self-imposed decision recorded elsewhere (the order's own terminal
/// `kRejected` state).
namespace aegis::participant::oms {

struct MissedTradeRecord {
  std::uint64_t client_order_id{0};
  std::int64_t missed_quantity_units{0};
};

class MissedTradeTracker {
 public:
  /// A no-op for `missed_quantity_units <= 0` -- a fully filled order has
  /// nothing to attribute, and this tracker never records a fabricated
  /// negative or zero-size miss.
  void record(std::uint64_t client_order_id, std::int64_t missed_quantity_units) {
    if (missed_quantity_units <= 0) {
      return;
    }
    total_missed_quantity_units_ += missed_quantity_units;
    records_.push_back(MissedTradeRecord{.client_order_id = client_order_id,
                                         .missed_quantity_units = missed_quantity_units});
  }

  [[nodiscard]] std::int64_t total_missed_quantity_units() const {
    return total_missed_quantity_units_;
  }
  [[nodiscard]] std::size_t missed_order_count() const { return records_.size(); }
  [[nodiscard]] const std::vector<MissedTradeRecord>& records() const { return records_; }

 private:
  std::vector<MissedTradeRecord> records_;
  std::int64_t total_missed_quantity_units_{0};
};

}  // namespace aegis::participant::oms
