#pragma once

#include <cstdint>

#include "cpp/events/exchange_messages.hpp"
#include "cpp/participant/book_builder/book_builder.hpp"
#include "cpp/statistics/rolling_zscore.hpp"

/// The first M4 strategy (AEGIS-076..081, AEGIS-004, AEGIS-080; ADR-0025).
///
/// `CalendarSpreadStrategy` proposes calendar-spread trades from reconstructed
/// near/far top-of-book state. It emits `StrategyProposal` values only: no
/// order id, no client-order-id, no OMS call, no exchange or gateway type
/// anywhere in this translation unit. `cpp-participant-strategy` may depend on
/// `[cpp-common, cpp-events, cpp-participant-book-builder, cpp-statistics]`
/// (`configs/architecture_rules.yaml`) and nothing else -- the composition
/// root (`cpp/participant/app`) is the only place a proposal is turned into an
/// order, through the existing mandatory risk seam and `OrderManager`
/// (ADR-0023).
namespace aegis::participant::strategy {

using Side = events::exchange::Side;

/// A fixed-size trading intent for one leg of the spread.
struct StrategyLeg {
  std::uint32_t instrument_id{0};
  Side side{Side::kBuy};
  std::int64_t quantity_units{0};
};

/// `has_action == false` means "no trade this update" -- a defined, common
/// case (flat and inside both thresholds, or a leg's book has no two-sided
/// market yet), never a partially-populated struct a caller must guess at.
struct StrategyProposal {
  bool has_action{false};
  StrategyLeg near;
  StrategyLeg far;
  /// `far_mid - near_mid` at the update that produced this proposal, and the
  /// leakage-free z-score `RollingZScore::push_and_score` returned for it
  /// (scored against the window as it stood *before* this observation, per
  /// ADR-0026) -- both reported for diagnostics/output even when
  /// `has_action` is false.
  double spread_price{0.0};
  double z_score{0.0};
};

/// AEGIS-004: buy the near leg / sell the far leg, or the reverse. Flat is the
/// only state a fresh strategy starts in; a position exits back to flat, never
/// through a third state -- this is a single fixed-size spread position, not a
/// ladder.
enum class SpreadPosition : std::uint8_t {
  kFlat = 0,
  kLongSpread = 1,   ///< Long near, short far.
  kShortSpread = 2,  ///< Short near, long far.
};

struct CalendarSpreadConfig {
  std::uint32_t near_instrument_id{0};
  std::uint32_t far_instrument_id{0};
  /// `RollingZScore`'s window: how many prior spread observations the score
  /// at each step is judged against (ADR-0026).
  std::size_t zscore_window{20};
  /// Enter when `|z| >= entry_threshold`; must be positive.
  double entry_threshold{2.0};
  /// Exit an open position when `|z| <= exit_threshold`; must be
  /// non-negative and less than `entry_threshold`, so the two never overlap.
  double exit_threshold{0.5};
  /// The fixed size traded on both legs, for both entry and exit -- this
  /// strategy holds at most one spread position at a time, never scales in.
  std::int64_t quantity_units{1};
};

class CalendarSpreadStrategy {
 public:
  explicit CalendarSpreadStrategy(CalendarSpreadConfig config) : config_(config) {}

  /// Called once per market-data update where both legs' reconstructed books
  /// have already absorbed it (AEGIS-080). Returns a no-action proposal,
  /// still carrying `z_score == 0.0`, when either leg's book has no mid price
  /// yet (`TopOfBook::mid_price_units` unset on either side) -- this update is
  /// not pushed into the z-score window in that case, so a leg that goes
  /// briefly one-sided cannot silently corrupt the spread history with a
  /// value it never actually observed.
  [[nodiscard]] StrategyProposal on_book_update(const book::TopOfBook& near,
                                                const book::TopOfBook& far);

  [[nodiscard]] SpreadPosition position() const { return position_; }

 private:
  CalendarSpreadConfig config_;
  stats::RollingZScore zscore_{config_.zscore_window};
  SpreadPosition position_{SpreadPosition::kFlat};
};

}  // namespace aegis::participant::strategy
