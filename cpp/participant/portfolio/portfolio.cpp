#include "cpp/participant/portfolio/portfolio.hpp"

#include <algorithm>

namespace aegis::participant::portfolio {
namespace {

[[nodiscard]] constexpr std::int64_t abs64(std::int64_t value) {
  return value < 0 ? -value : value;
}

}  // namespace

void Portfolio::apply_fill(std::uint32_t instrument_id, Side side, std::int64_t price_units,
                           std::int64_t quantity_units, std::int64_t fee_units) {
  Position& pos = positions_[instrument_id];

  const std::int64_t signed_delta = side == Side::kBuy ? quantity_units : -quantity_units;
  cash_units_ +=
      side == Side::kBuy ? -(price_units * quantity_units) : (price_units * quantity_units);
  cash_units_ -= fee_units;

  const bool opening_or_same_direction =
      pos.quantity_units == 0 || (pos.quantity_units > 0) == (signed_delta > 0);

  if (opening_or_same_direction) {
    const std::int64_t old_abs = abs64(pos.quantity_units);
    const std::int64_t add_abs = abs64(signed_delta);
    const std::int64_t new_abs = old_abs + add_abs;
    pos.average_price_units =
        new_abs == 0 ? 0 : (old_abs * pos.average_price_units + add_abs * price_units) / new_abs;
    pos.quantity_units += signed_delta;
    return;
  }

  // Reducing or reversing the existing direction: realize P&L on the
  // reduced quantity at the prior average price first.
  const std::int64_t old_abs = abs64(pos.quantity_units);
  const std::int64_t reduce_abs = old_abs < abs64(signed_delta) ? old_abs : abs64(signed_delta);
  const std::int64_t pnl_per_unit = pos.quantity_units > 0
                                        ? (price_units - pos.average_price_units)
                                        : (pos.average_price_units - price_units);
  pos.realized_pnl_units += pnl_per_unit * reduce_abs;

  pos.quantity_units += signed_delta;
  const std::int64_t remaining_abs = abs64(signed_delta) - reduce_abs;
  if (remaining_abs > 0) {
    // Flipped through zero: the excess opens a new position at this fill's
    // own price, never at the prior average.
    pos.average_price_units = price_units;
  } else if (pos.quantity_units == 0) {
    pos.average_price_units = 0;
  }
  // else: still the same direction, only reduced -- average_price_units is
  // unchanged by construction (reducing a position never moves its basis).
}

Position Portfolio::position(std::uint32_t instrument_id) const {
  const auto found = positions_.find(instrument_id);
  return found == positions_.end() ? Position{} : found->second;
}

std::int64_t Portfolio::unrealized_pnl_units(std::uint32_t instrument_id,
                                             std::int64_t mark_price_units) const {
  const Position pos = position(instrument_id);
  return pos.quantity_units * (mark_price_units - pos.average_price_units);
}

std::vector<std::pair<std::uint32_t, Position>> Portfolio::all_positions() const {
  std::vector<std::pair<std::uint32_t, Position>> positions(positions_.begin(), positions_.end());
  std::ranges::sort(positions, {}, &std::pair<std::uint32_t, Position>::first);
  return positions;
}

}  // namespace aegis::participant::portfolio
