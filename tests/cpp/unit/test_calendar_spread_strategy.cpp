#include <array>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/participant/book_builder/book_builder.hpp"
#include "cpp/participant/strategy/calendar_spread_strategy.hpp"

/// AEGIS-080, ADR-0026: leakage-free rolling z-score signal, and AEGIS-076's
/// downstream strategy behaviour built on it.
namespace {

using aegis::events::exchange::Side;
using aegis::participant::book::TopOfBook;
using aegis::participant::strategy::CalendarSpreadConfig;
using aegis::participant::strategy::CalendarSpreadStrategy;
using aegis::participant::strategy::SpreadPosition;
using aegis::participant::strategy::StrategyProposal;

TopOfBook make_top_of_book(double mid) {
  TopOfBook top;
  top.mid_price_units = mid;
  return top;
}

// AEGIS-080's leakage-free convention, exercised end to end: the very first
// observation can never be scored against itself, no matter how extreme it
// is -- RollingZScore::push_and_score's own documented edge case
// (`prior window has fewer than two observations`), not something the
// strategy layer re-derives.
TEST(CalendarSpreadStrategy, FirstObservationNeverScoresAgainstItself) {
  const CalendarSpreadConfig config{.near_instrument_id = 1,
                                    .far_instrument_id = 2,
                                    .zscore_window = 5,
                                    .entry_threshold = 0.5,
                                    .exit_threshold = 0.1,
                                    .quantity_units = 10};
  CalendarSpreadStrategy strategy(config);

  // A huge spread: if the observation could see itself, this would score as
  // an enormous outlier and cross even a tiny 0.5 threshold immediately.
  const auto proposal = strategy.on_book_update(make_top_of_book(100.0), make_top_of_book(1000.0));

  EXPECT_DOUBLE_EQ(proposal.z_score, 0.0);
  EXPECT_FALSE(proposal.has_action);
  EXPECT_EQ(strategy.position(), SpreadPosition::kFlat);
}

// A leg with no two-sided market yet must not corrupt the spread history
// with a value this strategy never actually observed.
TEST(CalendarSpreadStrategy, MissingMidOnEitherLegSkipsTheObservationEntirely) {
  const CalendarSpreadConfig config{.near_instrument_id = 1,
                                    .far_instrument_id = 2,
                                    .zscore_window = 5,
                                    .entry_threshold = 0.1,
                                    .exit_threshold = 0.01,
                                    .quantity_units = 1};
  CalendarSpreadStrategy strategy(config);

  const TopOfBook near = make_top_of_book(100.0);
  TopOfBook far_no_mid;  // No best bid/ask: mid_price_units unset.

  const auto skipped = strategy.on_book_update(near, far_no_mid);
  EXPECT_FALSE(skipped.has_action);
  EXPECT_DOUBLE_EQ(skipped.z_score, 0.0);

  // If the skipped update HAD been pushed, this would be the *second*
  // observation (count == 1, still < 2, still scores 0.0 either way) --
  // pick a value the two cases would disagree on: a genuinely two-sided
  // update that follows immediately still reports count-derived 0.0 here,
  // and a THIRD one afterward proves the skip: with the bad update excluded,
  // this third call is only the strategy's *second* real observation, so it
  // must still score exactly 0.0 (still < 2 real observations).
  const auto second_real = strategy.on_book_update(near, make_top_of_book(100.0));
  EXPECT_DOUBLE_EQ(second_real.z_score, 0.0);
  const auto third_real = strategy.on_book_update(near, make_top_of_book(100.5));
  EXPECT_DOUBLE_EQ(third_real.z_score, 0.0);  // Still only the 2nd real push scored.
}

// AEGIS-076/080: entry on the entry threshold, hold, exit on the exit
// threshold -- exact values cross-checked against
// tools/generate_calendar_spread_stream.py's own basis sequence, so this
// test also documents the arithmetic the committed demo fixture depends on.
TEST(CalendarSpreadStrategy, EntersOnEntryThresholdHoldsThenExitsOnExitThreshold) {
  const CalendarSpreadConfig config{.near_instrument_id = 10,
                                    .far_instrument_id = 20,
                                    .zscore_window = 20,
                                    .entry_threshold = 2.0,
                                    .exit_threshold = 0.5,
                                    .quantity_units = 7};
  CalendarSpreadStrategy strategy(config);

  const std::array<double, 6> spreads{0.50, 0.55, 0.60, 0.65, 2.50, 0.70};
  std::vector<StrategyProposal> proposals;
  std::vector<SpreadPosition> positions_after;  // strategy.position() right after each update.
  proposals.reserve(spreads.size());
  positions_after.reserve(spreads.size());
  for (const double spread : spreads) {
    proposals.push_back(strategy.on_book_update(make_top_of_book(0.0), make_top_of_book(spread)));
    positions_after.push_back(strategy.position());
  }

  EXPECT_FALSE(proposals[0].has_action);
  EXPECT_FALSE(proposals[1].has_action);

  ASSERT_TRUE(proposals[2].has_action);
  EXPECT_NEAR(proposals[2].z_score, 2.1213203435596393, 1e-9);
  EXPECT_EQ(positions_after[2], SpreadPosition::kShortSpread);
  EXPECT_EQ(proposals[2].near.side, Side::kSell);
  EXPECT_EQ(proposals[2].far.side, Side::kBuy);
  EXPECT_EQ(proposals[2].near.quantity_units, 7);
  EXPECT_EQ(proposals[2].far.quantity_units, 7);

  EXPECT_FALSE(proposals[3].has_action);  // z == 2.0 exactly: holding, not exiting.
  EXPECT_NEAR(proposals[3].z_score, 2.000000000000002, 1e-9);
  EXPECT_EQ(positions_after[3], SpreadPosition::kShortSpread);

  EXPECT_FALSE(proposals[4].has_action);  // A large outlier while already in position: no-op.
  EXPECT_EQ(positions_after[4], SpreadPosition::kShortSpread);

  ASSERT_TRUE(proposals[5].has_action);
  EXPECT_NEAR(proposals[5].z_score, -0.3013796514749198, 1e-9);
  EXPECT_EQ(positions_after[5], SpreadPosition::kFlat);
  EXPECT_EQ(proposals[5].near.side, Side::kBuy);  // Reverses the short spread: buy near.
  EXPECT_EQ(proposals[5].far.side, Side::kSell);
}

}  // namespace
