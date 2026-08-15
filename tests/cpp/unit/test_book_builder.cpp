#include <gtest/gtest.h>

#include "cpp/participant/book_builder/book_builder.hpp"
#include "tests/cpp/optional_access.hpp"

namespace {

using aegis::events::exchange::Side;
using aegis::events::market_data::BookDeltaEvent;
using aegis::events::market_data::BookLevelEntry;
using aegis::events::market_data::BookSnapshotEvent;
using aegis::events::market_data::DeltaKind;
using aegis::participant::book::BookBuilder;
using aegis::participant::book::PriceLevelView;

BookSnapshotEvent make_snapshot() {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = 1;
  snapshot.md_sequence = 1;
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 100, .quantity_units = 10, .order_id = 1});
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 99, .quantity_units = 20, .order_id = 2});
  snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kSell, .price_units = 101, .quantity_units = 15, .order_id = 3});
  return snapshot;
}

// AEGIS-064: snapshot fixtures reconstruct expected levels and quantities.
TEST(BookBuilder, ApplySnapshotReconstructsLevelsAndQuantities) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());

  EXPECT_EQ(book.best(Side::kBuy), (PriceLevelView{100, 10}));
  EXPECT_EQ(book.best(Side::kSell), (PriceLevelView{101, 15}));
  EXPECT_EQ(book.quantity_at(Side::kBuy, 99), 20);
  EXPECT_EQ(book.last_md_sequence(), 1U);
}

TEST(BookBuilder, LevelsReturnsBestFirstUpToDepth) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());
  const auto bids = book.levels(Side::kBuy, 5);
  ASSERT_EQ(bids.size(), 2U);
  EXPECT_EQ(bids[0], (PriceLevelView{100, 10}));
  EXPECT_EQ(bids[1], (PriceLevelView{99, 20}));
}

// AEGIS-066: order-level reconstruction -- add, modify, remove.
TEST(BookBuilder, OrderLifecycleFixturesReconstructOrderLevelState) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());

  BookDeltaEvent added;
  added.instrument_id = 1;
  added.md_sequence = 2;
  added.kind = DeltaKind::kOrderAdded;
  added.order_id = 4;
  added.side = Side::kBuy;
  added.price_units = 98;
  added.quantity_units = 5;
  book.apply_delta(added);

  ASSERT_TRUE(book.order(4).has_value());
  EXPECT_EQ(aegis::test::checked(book.order(4)),
            (aegis::participant::book::OrderView{Side::kBuy, 98, 5}));
  EXPECT_EQ(book.quantity_at(Side::kBuy, 98), 5);

  BookDeltaEvent modified;
  modified.instrument_id = 1;
  modified.md_sequence = 3;
  modified.kind = DeltaKind::kOrderModified;
  modified.order_id = 4;
  modified.side = Side::kBuy;
  modified.price_units = 98;
  modified.quantity_units = 2;
  book.apply_delta(modified);

  EXPECT_EQ(aegis::test::checked(book.order(4)).quantity_units, 2);
  EXPECT_EQ(book.quantity_at(Side::kBuy, 98), 2);

  BookDeltaEvent removed;
  removed.instrument_id = 1;
  removed.md_sequence = 4;
  removed.kind = DeltaKind::kOrderRemoved;
  removed.order_id = 4;
  removed.side = Side::kBuy;
  book.apply_delta(removed);

  EXPECT_FALSE(book.order(4).has_value());
  EXPECT_FALSE(book.quantity_at(Side::kBuy, 98).has_value());
  EXPECT_EQ(book.last_md_sequence(), 4U);
}

// AEGIS-067: price-level (aggregated-only) reconstruction -- no order_id ever
// supplied, exercising kPriceLevelSet exclusively.
TEST(BookBuilder, AggregatedOnlyDeltasReconstructPriceLevels) {
  BookBuilder book(1);

  BookDeltaEvent set_bid;
  set_bid.instrument_id = 1;
  set_bid.md_sequence = 1;
  set_bid.kind = DeltaKind::kPriceLevelSet;
  set_bid.side = Side::kBuy;
  set_bid.price_units = 100;
  set_bid.quantity_units = 50;
  book.apply_delta(set_bid);

  EXPECT_EQ(book.quantity_at(Side::kBuy, 100), 50);
  EXPECT_FALSE(book.order(0).has_value());  // No order identity in this mode.

  BookDeltaEvent clear_bid;
  clear_bid.instrument_id = 1;
  clear_bid.md_sequence = 2;
  clear_bid.kind = DeltaKind::kPriceLevelSet;
  clear_bid.side = Side::kBuy;
  clear_bid.price_units = 100;
  clear_bid.quantity_units = 0;
  book.apply_delta(clear_bid);

  EXPECT_FALSE(book.quantity_at(Side::kBuy, 100).has_value());
  EXPECT_FALSE(book.best(Side::kBuy).has_value());
}

TEST(BookBuilder, ApplySnapshotDiscardsPriorState) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());
  ASSERT_TRUE(book.best(Side::kBuy).has_value());

  BookSnapshotEvent fresh;
  fresh.instrument_id = 1;
  fresh.md_sequence = 10;
  book.apply_snapshot(fresh);

  EXPECT_FALSE(book.best(Side::kBuy).has_value());
  EXPECT_FALSE(book.best(Side::kSell).has_value());
  EXPECT_EQ(book.last_md_sequence(), 10U);
}

TEST(BookBuilder, ModifyingAnUnknownOrderIsIgnored) {
  BookBuilder book(1);
  BookDeltaEvent modify_unknown;
  modify_unknown.instrument_id = 1;
  modify_unknown.md_sequence = 1;
  modify_unknown.kind = DeltaKind::kOrderModified;
  modify_unknown.order_id = 999;
  book.apply_delta(modify_unknown);
  EXPECT_FALSE(book.order(999).has_value());
  EXPECT_FALSE(book.best(Side::kBuy).has_value());
}

// AEGIS-069: stale-data detection, on a virtual (never system) clock.
TEST(BookBuilder, StaleAfterElapsedTimeExceedsTheConfiguredThreshold) {
  BookBuilder book(1);
  book.configure_staleness(aegis::common::Duration{1000}, /*max_consecutive_faults=*/0);
  book.apply_snapshot(make_snapshot(), /*received_at_nanos=*/0);

  EXPECT_FALSE(book.is_stale(500));   // Within the threshold.
  EXPECT_FALSE(book.is_stale(1000));  // Exactly at the threshold: not yet stale.
  EXPECT_TRUE(book.is_stale(1001));   // Past it.
}

TEST(BookBuilder, StaleAfterEnoughConsecutiveSequenceFaults) {
  BookBuilder book(1);
  book.configure_staleness(aegis::common::Duration{0}, /*max_consecutive_faults=*/3);

  using aegis::participant::feed::SequenceDiagnostic;
  book.note_sequence_diagnostic(SequenceDiagnostic::kGap);
  book.note_sequence_diagnostic(SequenceDiagnostic::kGap);
  EXPECT_FALSE(book.is_stale(0));
  book.note_sequence_diagnostic(SequenceDiagnostic::kDuplicate);
  EXPECT_TRUE(book.is_stale(0));

  book.note_sequence_diagnostic(SequenceDiagnostic::kOk);
  EXPECT_FALSE(book.is_stale(0));  // An kOk observation resets the count.
}

TEST(BookBuilder, StalenessIsDisabledUntilConfigured) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot(), /*received_at_nanos=*/0);
  EXPECT_FALSE(book.is_stale(/*now_nanos=*/1'000'000'000'000));  // No threshold set: never stale.
}

// AEGIS-070/061: gap -> buffer -> snapshot -> re-base -> replay -> healthy state.
TEST(BookBuilder, RecoveryBuffersThenRebasesThenReplaysSurvivingDeltas) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());  // last_md_sequence == 1.
  ASSERT_FALSE(book.is_recovering());

  book.begin_recovery();
  ASSERT_TRUE(book.is_recovering());

  // Buffered, not applied: the book must not change while recovering.
  BookDeltaEvent stale_delta;
  stale_delta.instrument_id = 1;
  stale_delta.md_sequence = 2;  // At or below the eventual snapshot: discarded on recovery.
  stale_delta.kind = DeltaKind::kOrderAdded;
  stale_delta.order_id = 99;
  stale_delta.side = Side::kBuy;
  stale_delta.price_units = 50;
  stale_delta.quantity_units = 1;
  book.apply_delta(stale_delta);
  EXPECT_EQ(book.quantity_at(Side::kBuy, 50), std::nullopt);  // Buffered, not applied.

  BookDeltaEvent surviving_delta;
  surviving_delta.instrument_id = 1;
  surviving_delta.md_sequence = 6;  // Above the eventual snapshot: replayed.
  surviving_delta.kind = DeltaKind::kOrderAdded;
  surviving_delta.order_id = 100;
  surviving_delta.side = Side::kBuy;
  surviving_delta.price_units = 60;
  surviving_delta.quantity_units = 7;
  book.apply_delta(surviving_delta);

  BookSnapshotEvent recovery_snapshot;
  recovery_snapshot.instrument_id = 1;
  recovery_snapshot.md_sequence = 5;  // Covers everything up to and including sequence 5.
  recovery_snapshot.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 200, .quantity_units = 9, .order_id = 1});
  book.apply_snapshot(recovery_snapshot);

  EXPECT_FALSE(book.is_recovering());
  EXPECT_EQ(book.quantity_at(Side::kBuy, 200), 9);             // From the recovery snapshot.
  EXPECT_EQ(book.quantity_at(Side::kBuy, 60), 7);              // Replayed: sequence 6 > 5.
  EXPECT_FALSE(book.quantity_at(Side::kBuy, 50).has_value());  // Discarded: sequence 2 <= 5.
  EXPECT_EQ(book.last_md_sequence(), 6U);  // The replayed delta is the latest applied state.
}

// AEGIS-071: top-of-book metrics.
TEST(BookBuilder, TopOfBookReportsBestBidAskSpreadAndMid) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());  // best bid 100/10, best ask 101/15.

  const auto top = book.top_of_book();
  ASSERT_TRUE(top.best_bid.has_value());
  ASSERT_TRUE(top.best_ask.has_value());
  EXPECT_EQ(aegis::test::checked(top.best_bid), (PriceLevelView{100, 10}));
  EXPECT_EQ(aegis::test::checked(top.best_ask), (PriceLevelView{101, 15}));
  ASSERT_TRUE(top.spread_units.has_value());
  EXPECT_EQ(aegis::test::checked(top.spread_units), 1);
  ASSERT_TRUE(top.mid_price_units.has_value());
  EXPECT_DOUBLE_EQ(aegis::test::checked(top.mid_price_units), 100.5);
}

TEST(BookBuilder, TopOfBookLeavesSpreadAndMidUnsetWithOnlyOneSide) {
  BookBuilder book(1);
  BookSnapshotEvent bid_only;
  bid_only.instrument_id = 1;
  bid_only.md_sequence = 1;
  bid_only.entries.push_back(
      BookLevelEntry{.side = Side::kBuy, .price_units = 100, .quantity_units = 10, .order_id = 1});
  book.apply_snapshot(bid_only);

  const auto top = book.top_of_book();
  EXPECT_TRUE(top.best_bid.has_value());
  EXPECT_FALSE(top.best_ask.has_value());
  EXPECT_FALSE(top.spread_units.has_value());
  EXPECT_FALSE(top.mid_price_units.has_value());
}

// AEGIS-072: quantity-weighted microprice.
TEST(BookBuilder, MicropriceWeightsTowardTheOppositeSidesPrice) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());  // bid 100 qty10, ask 101 qty15.

  const auto price = book.microprice();
  ASSERT_TRUE(price.has_value());
  // (10*101 + 15*100) / 25 = (1010+1500)/25 = 100.4.
  EXPECT_DOUBLE_EQ(aegis::test::checked(price), ((10.0 * 101.0) + (15.0 * 100.0)) / 25.0);
}

TEST(BookBuilder, MicropriceIsUndefinedWithoutBothSides) {
  BookBuilder book(1);
  EXPECT_FALSE(book.microprice().has_value());
}

// AEGIS-073: depth/quantity imbalance.
TEST(BookBuilder, DepthImbalanceReflectsHeavierBidSide) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());  // bids: 100/10, 99/20 = 30; ask: 101/15.

  const auto imbalance = book.depth_imbalance(5);
  ASSERT_TRUE(imbalance.has_value());
  // (30 - 15) / (30 + 15) = 1/3.
  EXPECT_DOUBLE_EQ(aegis::test::checked(imbalance), (30.0 - 15.0) / (30.0 + 15.0));
}

TEST(BookBuilder, DepthImbalanceRespectsTheRequestedDepth) {
  BookBuilder book(1);
  book.apply_snapshot(make_snapshot());  // bids: 100/10 (top), 99/20; ask: 101/15.

  const auto top_only = book.depth_imbalance(1);
  ASSERT_TRUE(top_only.has_value());
  // (10 - 15) / (10 + 15) = -1/5, using only the top bid level.
  EXPECT_DOUBLE_EQ(aegis::test::checked(top_only), (10.0 - 15.0) / (10.0 + 15.0));
}

TEST(BookBuilder, DepthImbalanceUndefinedForZeroDepthOrAnEmptyBook) {
  BookBuilder book(1);
  EXPECT_FALSE(book.depth_imbalance(0).has_value());
  EXPECT_FALSE(book.depth_imbalance(5).has_value());  // Empty book.
}

// AEGIS-075: queue depletion.
TEST(BookBuilder, QueueDepletionTracksConsumptionAgainstTheHighWaterMark) {
  BookBuilder book(1);
  BookDeltaEvent add;
  add.instrument_id = 1;
  add.md_sequence = 1;
  add.kind = DeltaKind::kOrderAdded;
  add.order_id = 1;
  add.side = Side::kBuy;
  add.price_units = 100;
  add.quantity_units = 20;
  book.apply_delta(add);

  auto signal = book.queue_depletion(Side::kBuy, 100);
  ASSERT_TRUE(signal.has_value());
  EXPECT_EQ(aegis::test::checked(signal).original_quantity_units, 20);
  EXPECT_EQ(aegis::test::checked(signal).current_quantity_units, 20);
  EXPECT_DOUBLE_EQ(aegis::test::checked(signal).depletion_ratio, 0.0);

  BookDeltaEvent modify;
  modify.instrument_id = 1;
  modify.md_sequence = 2;
  modify.kind = DeltaKind::kOrderModified;
  modify.order_id = 1;
  modify.side = Side::kBuy;
  modify.price_units = 100;
  modify.quantity_units = 5;  // 15 of the original 20 consumed.
  book.apply_delta(modify);

  signal = book.queue_depletion(Side::kBuy, 100);
  ASSERT_TRUE(signal.has_value());
  EXPECT_EQ(aegis::test::checked(signal).original_quantity_units,
            20);  // High-water mark unchanged by a decrease.
  EXPECT_EQ(aegis::test::checked(signal).current_quantity_units, 5);
  EXPECT_DOUBLE_EQ(aegis::test::checked(signal).depletion_ratio, 1.0 - (5.0 / 20.0));
}

TEST(BookBuilder, QueueDepletionIsUndefinedForALevelThatDoesNotExist) {
  BookBuilder book(1);
  EXPECT_FALSE(book.queue_depletion(Side::kBuy, 100).has_value());
}

// AEGIS-075: post-fill adverse selection.
TEST(BookBuilder, AdverseSelectionDetectsAnUnfavorableMoveAfterTheWindow) {
  BookBuilder book(1);
  // A single bid level, so a kPriceLevelSet on it unambiguously moves best bid.
  BookDeltaEvent open_bid;
  open_bid.instrument_id = 1;
  open_bid.md_sequence = 1;
  open_bid.kind = DeltaKind::kPriceLevelSet;
  open_bid.side = Side::kBuy;
  open_bid.price_units = 100;
  open_bid.quantity_units = 10;
  book.apply_delta(open_bid);

  // A resting bid was filled at 100; judged against the best bid 2 updates later.
  book.record_fill_for_adverse_selection(Side::kBuy, 100, /*window_updates=*/2);

  BookDeltaEvent unrelated_tick;
  unrelated_tick.instrument_id = 1;
  unrelated_tick.md_sequence = 2;
  unrelated_tick.kind = DeltaKind::kPriceLevelSet;
  unrelated_tick.side = Side::kSell;
  unrelated_tick.price_units = 200;
  unrelated_tick.quantity_units = 1;
  book.apply_delta(unrelated_tick);
  EXPECT_TRUE(book.drain_resolved_adverse_selection().empty());  // 1 tick elapsed, not yet 2.

  // The bid moves to 90 -- adverse for whoever was filled (bought) at 100.
  BookDeltaEvent bid_drop;
  bid_drop.instrument_id = 1;
  bid_drop.md_sequence = 3;
  bid_drop.kind = DeltaKind::kPriceLevelSet;
  bid_drop.side = Side::kBuy;
  bid_drop.price_units = 90;
  bid_drop.quantity_units = 10;
  book.apply_delta(bid_drop);
  BookDeltaEvent clear_100;
  clear_100.instrument_id = 1;
  clear_100.md_sequence = 4;
  clear_100.kind = DeltaKind::kPriceLevelSet;
  clear_100.side = Side::kBuy;
  clear_100.price_units = 100;
  clear_100.quantity_units = 0;
  book.apply_delta(clear_100);

  const auto resolved = book.drain_resolved_adverse_selection();
  ASSERT_EQ(resolved.size(), 1U);
  EXPECT_EQ(resolved[0].fill_side, Side::kBuy);
  EXPECT_EQ(resolved[0].fill_price_units, 100);
  ASSERT_TRUE(resolved[0].mark_price_units.has_value());
  EXPECT_EQ(aegis::test::checked(resolved[0].mark_price_units), 90);
  EXPECT_TRUE(resolved[0].adverse);

  // Drained: does not resolve a second time.
  EXPECT_TRUE(book.drain_resolved_adverse_selection().empty());
}

TEST(BookBuilder, AdverseSelectionIsFalseWhenTheMarkIsFavorable) {
  BookBuilder book(1);
  BookDeltaEvent open_bid;
  open_bid.instrument_id = 1;
  open_bid.md_sequence = 1;
  open_bid.kind = DeltaKind::kPriceLevelSet;
  open_bid.side = Side::kBuy;
  open_bid.price_units = 100;
  open_bid.quantity_units = 10;
  book.apply_delta(open_bid);

  book.record_fill_for_adverse_selection(Side::kBuy, 100, /*window_updates=*/1);

  // A new, higher bid level becomes best; the fill's own price level is
  // untouched (its quantity is irrelevant to this check, only best() is).
  BookDeltaEvent bid_rise;
  bid_rise.instrument_id = 1;
  bid_rise.md_sequence = 2;
  bid_rise.kind = DeltaKind::kPriceLevelSet;
  bid_rise.side = Side::kBuy;
  bid_rise.price_units = 110;
  bid_rise.quantity_units = 10;
  book.apply_delta(bid_rise);

  const auto resolved = book.drain_resolved_adverse_selection();
  ASSERT_EQ(resolved.size(), 1U);
  EXPECT_FALSE(resolved[0].adverse);  // 110 > 100: favorable, not adverse, for a buy fill.
}

TEST(BookBuilder, AdverseSelectionMarkIsUnsetWhenTheSideIsEmptyAtEvaluation) {
  BookBuilder book(1);
  book.record_fill_for_adverse_selection(Side::kSell, 100, /*window_updates=*/1);

  BookDeltaEvent noop;
  noop.instrument_id = 1;
  noop.md_sequence = 1;
  noop.kind = DeltaKind::kPriceLevelSet;
  noop.side = Side::kBuy;
  noop.price_units = 50;
  noop.quantity_units = 1;
  book.apply_delta(noop);  // The ask side stays empty throughout.

  const auto resolved = book.drain_resolved_adverse_selection();
  ASSERT_EQ(resolved.size(), 1U);
  EXPECT_FALSE(resolved[0].mark_price_units.has_value());
  EXPECT_FALSE(resolved[0].adverse);
}

}  // namespace
