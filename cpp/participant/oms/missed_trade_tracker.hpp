#pragma once

#include <cstdint>
#include <vector>

#include "cpp/events/exchange_messages.hpp"

/// Missed-trade attribution (AEGIS-117; ADR-0023).
///
/// Records the untraded remainder of orders that ended without filling, and
/// -- against a caller-supplied mark -- what not trading them cost.
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
  /// The price the missed order would have traded at, and its side -- kept so
  /// opportunity cost can be computed later against a mark the caller
  /// supplies. Recording the order's own price is a fact this module already
  /// holds; choosing a mark is not, which is why the mark is a parameter of
  /// the query rather than a field here (AEGIS-117).
  std::int64_t order_price_units{0};
  events::exchange::Side side{events::exchange::Side::kBuy};

  /// Signed opportunity cost of not trading, in price units x quantity:
  /// positive when the market moved the way the order wanted (so missing it
  /// cost something), negative when missing it was in fact favourable. Zero
  /// for an order with no price -- a market order has no reference the cost
  /// could be measured against, and inventing one would fabricate a market
  /// fact.
  [[nodiscard]] std::int64_t opportunity_cost_units(std::int64_t mark_price_units) const {
    if (order_price_units == 0) {
      return 0;
    }
    const std::int64_t per_unit = side == events::exchange::Side::kBuy
                                      ? (mark_price_units - order_price_units)
                                      : (order_price_units - mark_price_units);
    return per_unit * missed_quantity_units;
  }
};

class MissedTradeTracker {
 public:
  /// A no-op for `missed_quantity_units <= 0` -- a fully filled order has
  /// nothing to attribute, and this tracker never records a fabricated
  /// negative or zero-size miss.
  void record(std::uint64_t client_order_id, std::int64_t missed_quantity_units,
              std::int64_t order_price_units = 0,
              events::exchange::Side side = events::exchange::Side::kBuy) {
    if (missed_quantity_units <= 0) {
      return;
    }
    total_missed_quantity_units_ += missed_quantity_units;
    records_.push_back(MissedTradeRecord{.client_order_id = client_order_id,
                                         .missed_quantity_units = missed_quantity_units,
                                         .order_price_units = order_price_units,
                                         .side = side});
  }

  /// Total signed opportunity cost across every recorded miss, against one
  /// caller-supplied mark. The mark is the caller's to choose: this module
  /// observes no market and will not invent one.
  [[nodiscard]] std::int64_t total_opportunity_cost_units(std::int64_t mark_price_units) const {
    std::int64_t total = 0;
    for (const auto& record : records_) {
      total += record.opportunity_cost_units(mark_price_units);
    }
    return total;
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
