#pragma once

#include <cstdint>
#include <optional>

#include "cpp/exchange/order_book/types.hpp"

/// Instrument price/quantity grid (ADR-0009 §4.4, ADR-0010).
///
/// Prices and quantities are integers in the smallest representable unit; the
/// instrument declares the grid on top of them, which is what makes tick and
/// lot validation genuine rather than tautological. Every accepted quantity is
/// a multiple of `lot_size_units`; a fill is `min(remaining_maker,
/// remaining_taker)`, and the minimum of two multiples of L is itself a
/// multiple of L, so every fill, every residual and every level aggregate
/// stays a lot multiple by construction (P4, `tests/cpp/property/test_quantity_conservation.cpp`).
namespace aegis::exchange {

struct InstrumentSpec {
  InstrumentId instrument_id;
  PriceUnits price_floor_units;     ///< Grid origin and inclusive lower bound.
  PriceUnits price_ceiling_units;   ///< Inclusive upper bound.
  std::int64_t tick_size_units{1};  ///< > 1 in fixtures, so tick validation is not vacuous.
  QuantityUnits min_quantity_units;
  QuantityUnits max_quantity_units;
  std::int64_t lot_size_units{1};  ///< > 1 in fixtures, so lot validation is not vacuous.

  /// Number of distinct on-grid prices in [floor, ceiling] — the finite,
  /// documented price-domain cardinality AEGIS-038 requires.
  [[nodiscard]] std::int64_t price_domain_size() const {
    return ((price_ceiling_units.value() - price_floor_units.value()) / tick_size_units) + 1;
  }
};

/// Checked in this fixed order (ADR-0009 §4.4): out-of-band before off-tick,
/// so a price that fails both reports the boundary violation, which is the
/// coarser and more actionable of the two.
[[nodiscard]] std::optional<RejectReason> validate_price(const InstrumentSpec& spec,
                                                         PriceUnits price);

/// Checked in this fixed order: nonpositive before off-lot before out-of-range.
[[nodiscard]] std::optional<RejectReason> validate_quantity(const InstrumentSpec& spec,
                                                            QuantityUnits quantity);

}  // namespace aegis::exchange
