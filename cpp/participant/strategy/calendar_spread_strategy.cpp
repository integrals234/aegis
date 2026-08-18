#include "cpp/participant/strategy/calendar_spread_strategy.hpp"

#include <cmath>

namespace aegis::participant::strategy {

namespace {

/// The opposite side of a spread's near leg is always the far leg's side --
/// this strategy never trades both legs the same direction.
[[nodiscard]] Side opposite(Side side) { return side == Side::kBuy ? Side::kSell : Side::kBuy; }

}  // namespace

StrategyProposal CalendarSpreadStrategy::on_book_update(const book::TopOfBook& near,
                                                        const book::TopOfBook& far) {
  StrategyProposal proposal;
  if (!near.mid_price_units.has_value() || !far.mid_price_units.has_value()) {
    // A leg with no two-sided market yet contributes no observation: pushing
    // one here would silently record a spread this strategy never actually
    // observed (see header).
    return proposal;
  }

  const double spread = *far.mid_price_units - *near.mid_price_units;
  const double z = zscore_.push_and_score(spread);
  proposal.spread_price = spread;
  proposal.z_score = z;

  const double abs_z = std::fabs(z);

  if (position_ == SpreadPosition::kFlat) {
    if (z <= -config_.entry_threshold) {
      // The spread is unusually narrow (or negative) versus its recent
      // history: buy the near leg, sell the far leg, expecting the spread to
      // widen back toward its recent mean.
      position_ = SpreadPosition::kLongSpread;
      proposal.has_action = true;
      proposal.near = StrategyLeg{.instrument_id = config_.near_instrument_id,
                                  .side = Side::kBuy,
                                  .quantity_units = config_.quantity_units};
      proposal.far = StrategyLeg{.instrument_id = config_.far_instrument_id,
                                 .side = Side::kSell,
                                 .quantity_units = config_.quantity_units};
    } else if (z >= config_.entry_threshold) {
      // The spread is unusually wide: sell the near leg, buy the far leg,
      // expecting reversion the other way.
      position_ = SpreadPosition::kShortSpread;
      proposal.has_action = true;
      proposal.near = StrategyLeg{.instrument_id = config_.near_instrument_id,
                                  .side = Side::kSell,
                                  .quantity_units = config_.quantity_units};
      proposal.far = StrategyLeg{.instrument_id = config_.far_instrument_id,
                                 .side = Side::kBuy,
                                 .quantity_units = config_.quantity_units};
    }
    return proposal;
  }

  // Already in a position: only ever exit (flatten), never add to it or flip
  // directly -- a flip would need two proposals worth of risk decisions in
  // one update, which this fixed-size, single-position strategy does not
  // attempt. Exiting closes both legs at the position's fixed size, with each
  // leg's side reversed from how it was opened.
  if (abs_z <= config_.exit_threshold) {
    const bool was_long = position_ == SpreadPosition::kLongSpread;
    position_ = SpreadPosition::kFlat;
    proposal.has_action = true;
    proposal.near = StrategyLeg{.instrument_id = config_.near_instrument_id,
                                .side = was_long ? Side::kSell : Side::kBuy,
                                .quantity_units = config_.quantity_units};
    proposal.far = StrategyLeg{.instrument_id = config_.far_instrument_id,
                               .side = opposite(proposal.near.side),
                               .quantity_units = config_.quantity_units};
  }
  return proposal;
}

}  // namespace aegis::participant::strategy
