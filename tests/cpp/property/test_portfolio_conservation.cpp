#include <cstdint>
#include <random>

#include <gtest/gtest.h>

#include "cpp/participant/portfolio/portfolio.hpp"

/// AEGIS-118 (complete): double-entry-style reconciliation as a property
/// over a generated fill sequence, rather than only hand-computed scenarios
/// (`tests/cpp/unit/test_portfolio.cpp`).
///
/// The invariant checked after every fill, for one instrument starting
/// flat:
///
///   cash_units + quantity_units * average_price_units - realized_pnl_units
///     == -(cumulative fees paid)
///
/// Read left to right: cash already paid out, plus what the still-open
/// position would be worth if unwound at its own average cost (i.e. no
/// further gain or loss), minus P&L already realized, must equal exactly
/// the negative of fees paid -- nothing else ever enters or leaves the
/// ledger.
///
/// `Portfolio::apply_fill` computes `average_price_units` with **integer**
/// division when a fill adds to an already-open position in the same
/// direction (`(old_abs * old_avg + add_abs * price) / new_abs`) -- a
/// deliberate, documented truncation, consistent with every other integer
/// price/quantity field in this codebase, and it compounds across repeated
/// same-direction adds. That makes the identity above only *approximately*
/// true across an arbitrary fill sequence, which would force this test to
/// either duplicate `apply_fill`'s own rounding as a hand-rolled oracle or
/// accept a loose, unprincipled tolerance -- neither is a real property.
///
/// So the generator here deliberately never adds to an already-open
/// position in the same direction: every fill either opens a flat position
/// (a single fill's own price, exactly divisible by construction) or
/// reduces/flips an open one (a pure subtraction, or a flip that is set
/// directly to the flipping fill's own price -- see `apply_fill`'s "Flipped
/// through zero" branch). Both paths are exact under integer division, so
/// the identity holds bit-for-bit while still exercising realized P&L,
/// partial reduction, full flattening and flip-through-zero across many
/// random sides, prices, quantities and fees.
namespace {

using aegis::events::exchange::Side;
using aegis::participant::portfolio::Portfolio;
using aegis::participant::portfolio::Position;

void check_conservation(const Portfolio& ledger, std::uint32_t instrument_id,
                        std::int64_t cumulative_fees_units) {
  const Position pos = ledger.position(instrument_id);
  const std::int64_t lhs =
      ledger.cash_units() + (pos.quantity_units * pos.average_price_units) - pos.realized_pnl_units;
  EXPECT_EQ(lhs, -cumulative_fees_units);
}

TEST(PortfolioConservation, SingleInstrumentHoldsOverAGeneratedOpenReduceFlipSequence) {
  // Fixed seed is deliberate (AEGIS-005): reproducibility is the point.
  std::mt19937 rng{2026};  // NOLINT(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::uniform_int_distribution<int> side_choice(0, 1);
  std::uniform_int_distribution<std::int64_t> price_dist(900, 1100);
  std::uniform_int_distribution<std::int64_t> quantity_dist(1, 50);
  std::uniform_int_distribution<std::int64_t> fee_dist(0, 5);

  constexpr std::uint32_t kInstrument = 1;
  Portfolio ledger;
  std::int64_t cumulative_fees = 0;

  constexpr int kFills = 500;
  for (int i = 0; i < kFills; ++i) {
    const std::int64_t current_quantity = ledger.position(kInstrument).quantity_units;
    // Never add to an already-open position in the same direction: open
    // from flat with either side, or always trade the opposite side of
    // whatever is currently open (a reduce or a flip, per the header note).
    const Side opening_side = side_choice(rng) == 0 ? Side::kBuy : Side::kSell;
    const Side closing_side = current_quantity > 0 ? Side::kSell : Side::kBuy;
    const Side side = current_quantity == 0 ? opening_side : closing_side;
    const std::int64_t price = price_dist(rng);
    const std::int64_t quantity = quantity_dist(rng);
    const std::int64_t fee = fee_dist(rng);

    ledger.apply_fill(kInstrument, side, price, quantity, fee);
    cumulative_fees += fee;

    check_conservation(ledger, kInstrument, cumulative_fees);
  }
}

TEST(PortfolioConservation, MultipleInstrumentsShareOneCashPoolButEachReconcilesTogether) {
  std::mt19937 rng{4104};  // NOLINT(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::uniform_int_distribution<int> side_choice(0, 1);
  std::uniform_int_distribution<std::uint32_t> instrument_dist(1, 4);
  std::uniform_int_distribution<std::int64_t> price_dist(900, 1100);
  std::uniform_int_distribution<std::int64_t> quantity_dist(1, 50);
  std::uniform_int_distribution<std::int64_t> fee_dist(0, 5);

  constexpr std::uint32_t kMaxInstrument = 4;
  Portfolio ledger;
  std::int64_t cumulative_fees = 0;

  constexpr int kFills = 500;
  for (int i = 0; i < kFills; ++i) {
    const std::uint32_t instrument = instrument_dist(rng);
    const std::int64_t current_quantity = ledger.position(instrument).quantity_units;
    const Side opening_side = side_choice(rng) == 0 ? Side::kBuy : Side::kSell;
    const Side closing_side = current_quantity > 0 ? Side::kSell : Side::kBuy;
    const Side side = current_quantity == 0 ? opening_side : closing_side;
    const std::int64_t price = price_dist(rng);
    const std::int64_t quantity = quantity_dist(rng);
    const std::int64_t fee = fee_dist(rng);

    ledger.apply_fill(instrument, side, price, quantity, fee);
    cumulative_fees += fee;

    // The global cash pool plus every instrument's own cost-basis-at-average
    // and realized P&L must reconcile together against total fees -- fees
    // are the only leak out of the combined ledger.
    std::int64_t combined_lhs = ledger.cash_units();
    for (std::uint32_t instrument_id = 1; instrument_id <= kMaxInstrument; ++instrument_id) {
      const Position pos = ledger.position(instrument_id);
      combined_lhs += pos.quantity_units * pos.average_price_units;
      combined_lhs -= pos.realized_pnl_units;
    }
    EXPECT_EQ(combined_lhs, -cumulative_fees);
  }
}

}  // namespace
