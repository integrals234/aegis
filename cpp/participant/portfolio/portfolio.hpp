#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cpp/events/exchange_messages.hpp"

/// Position and cash accounting foundation (AEGIS-118).
///
/// `cpp-participant-portfolio.may_depend_on = [cpp-common, cpp-events]` only
/// (`configs/architecture_rules.yaml`) -- it has no edge to the OMS. Portfolio
/// state changes only from a caller-supplied fill, matching
/// `docs/ARCHITECTURE.md` rule 5 ("Portfolio state changes only from accepted
/// execution/account events"); this class does not decide whether a fill
/// happened, only what a fill implies about position, average price, realized
/// P&L, and cash.
namespace aegis::participant::portfolio {

using Side = events::exchange::Side;

/// One instrument's position. `quantity_units` is signed: positive is long,
/// negative is short, zero is flat. `average_price_units` is the
/// volume-weighted average entry price of the *current* position and is
/// meaningless (reported as 0) while flat.
struct Position {
  std::int64_t quantity_units{0};
  std::int64_t average_price_units{0};
  std::int64_t realized_pnl_units{0};  ///< Cumulative, across every fill so far.

  friend bool operator==(const Position&, const Position&) = default;
};

class Portfolio {
 public:
  Portfolio() = default;

  /// Restoring constructor (AEGIS-237; ADR-0024): rebuilds cash and every
  /// instrument's position directly from already-computed state, bypassing
  /// `apply_fill`'s own accounting arithmetic entirely -- those fills
  /// already happened in the run that produced the snapshot, and replaying
  /// them again would double-count. `positions` may list a given
  /// `instrument_id` at most once; the caller (the snapshot codec) is
  /// responsible for that, since this constructor has no way to signal a
  /// rejection.
  Portfolio(std::int64_t cash_units, std::vector<std::pair<std::uint32_t, Position>> positions)
      : cash_units_(cash_units) {
    for (auto& [instrument_id, position] : positions) {
      positions_.emplace(instrument_id, std::move(position));
    }
  }

  /// Applies one fill: `side` is the participant's own side of the trade
  /// (a buy adds to a long position or reduces/covers a short one).
  /// `price_units` and `quantity_units` are always positive; `quantity_units`
  /// is this fill's size, not the order's total. `fee_units` is subtracted
  /// from cash regardless of side.
  ///
  /// Average-cost accounting: a fill that adds to the existing direction (or
  /// opens a flat position) updates the volume-weighted average price and
  /// leaves realized P&L untouched. A fill that reduces or reverses the
  /// existing direction realizes P&L on the reduced quantity at the prior
  /// average price first; if the fill's quantity exceeds what was open, the
  /// position flips and the excess opens a new position at this fill's price
  /// — never at the old average, which would misattribute P&L across two
  /// unrelated positions.
  void apply_fill(std::uint32_t instrument_id, Side side, std::int64_t price_units,
                  std::int64_t quantity_units, std::int64_t fee_units = 0);

  [[nodiscard]] Position position(std::uint32_t instrument_id) const;
  [[nodiscard]] std::int64_t cash_units() const { return cash_units_; }

  /// `quantity_units * (mark_price_units - average_price_units)` — correct
  /// for both a long (positive quantity) and a short (negative quantity)
  /// position under one formula, with no sign-dependent branch to get wrong.
  [[nodiscard]] std::int64_t unrealized_pnl_units(std::uint32_t instrument_id,
                                                  std::int64_t mark_price_units) const;

  /// AEGIS-237; ADR-0024 capture support. Every instrument that has had at
  /// least one fill applied, in ascending `instrument_id` order --
  /// `positions_` is unordered, and a snapshot's bytes must not depend on
  /// hash-table iteration order (`docs/RECOVERY_CONTRACT.md` byte-stability
  /// obligation). An instrument only ever queried via `position()` and
  /// never filled is not included: `position()` uses `find`, not
  /// `operator[]`, so it never inserts a phantom flat entry.
  [[nodiscard]] std::vector<std::pair<std::uint32_t, Position>> all_positions() const;

 private:
  std::unordered_map<std::uint32_t, Position> positions_;
  std::int64_t cash_units_{0};
};

}  // namespace aegis::participant::portfolio
