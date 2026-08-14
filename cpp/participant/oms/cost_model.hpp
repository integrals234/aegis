#pragma once

#include <cstdint>

#include "cpp/events/exchange_messages.hpp"

/// Fees and slippage attribution (AEGIS-116; ADR-0023).
///
/// Both pieces are pure arithmetic over already-known fill data (a
/// committed fee rate, a reference price the caller already decided on) --
/// nothing here invents a market fact or a benchmark figure
/// (`docs/BENCHMARK_POLICY.md`/`docs/CV_CLAIMS_POLICY.md` do not apply).
namespace aegis::participant::oms {

using Side = events::exchange::Side;

/// A fixed, committed fee rate, expressed in millionths of notional (parts
/// per million) so it stays exact integer arithmetic -- never a floating
/// rate silently drifting through repeated fills. 1000 ppm == 0.1%.
struct FeeSchedule {
  std::int64_t fee_rate_ppm{0};

  /// Rounds toward zero (integer division), which is the same convention
  /// `Portfolio` and the rest of the participant's integer price/quantity
  /// arithmetic already uses -- there is no fractional currency unit here.
  [[nodiscard]] std::int64_t fee_units(std::int64_t price_units,
                                       std::int64_t quantity_units) const {
    return (price_units * quantity_units * fee_rate_ppm) / 1'000'000;
  }
};

/// One fill's slippage against whatever reference price the caller already
/// had in hand (a limit price, a decision-time quote) -- this struct does
/// not decide what "reference" means, only reports the signed difference.
///
/// `slippage_units` is signed per unit traded, from the participant's own
/// point of view: positive is adverse (the fill cost more than the
/// reference for a buy, or paid less than the reference for a sell),
/// negative is favorable. The sign flips with `side` so a caller never has
/// to branch on side again downstream.
struct SlippageResult {
  std::int64_t reference_price_units{0};
  std::int64_t fill_price_units{0};
  std::int64_t slippage_units{0};
};

[[nodiscard]] inline SlippageResult compute_slippage(Side side, std::int64_t reference_price_units,
                                                     std::int64_t fill_price_units) {
  const std::int64_t signed_diff = side == Side::kBuy ? (fill_price_units - reference_price_units)
                                                      : (reference_price_units - fill_price_units);
  return SlippageResult{.reference_price_units = reference_price_units,
                        .fill_price_units = fill_price_units,
                        .slippage_units = signed_diff};
}

}  // namespace aegis::participant::oms
