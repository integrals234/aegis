#include <cstddef>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/participant/app/participant_snapshot.hpp"
#include "cpp/participant/oms/oms_snapshot.hpp"
#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/oms/recorded_response_adapter.hpp"
#include "cpp/participant/portfolio/portfolio_snapshot.hpp"

/// AEGIS-237; ADR-0024: participant-state snapshot codec -- the
/// participant-side analogue of `test_snapshot_roundtrip.cpp`. Covers the
/// three codecs (`oms`, `portfolio`, and the outer `app` composition) each
/// against: round-trip equality, byte stability, unknown-version rejection
/// and malformed/truncated-payload rejection; then OMS lifecycle and
/// portfolio accounting continuity across `OrderManager`'s and
/// `Portfolio`'s restoring constructors. Process-boundary continuation
/// equality across two real `aegis_participant_run` invocations is proven
/// separately in `tests/replay/test_participant_recovery.py` -- this file
/// is the in-process half of the same proof, exactly as
/// `test_snapshot_roundtrip.cpp` is for `ExchangeSnapshot`.
namespace {

using aegis::events::exchange::OrderAcceptedEvent;
using aegis::events::exchange::OrderTerminatedEvent;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::events::exchange::TerminationReason;
using aegis::participant::app::capture_participant_snapshot;
using aegis::participant::app::ParticipantSnapshot;
using aegis::participant::app::ParticipantSnapshotError;
using aegis::participant::app::read_participant_snapshot;
using aegis::participant::app::write_participant_snapshot;
using aegis::participant::oms::capture_oms_snapshot;
using aegis::participant::oms::OmsOrderRecord;
using aegis::participant::oms::OmsSnapshot;
using aegis::participant::oms::OmsSnapshotError;
using aegis::participant::oms::OrderManager;
using aegis::participant::oms::OrderState;
using aegis::participant::oms::read_oms_snapshot;
using aegis::participant::oms::RecordedResponseAdapter;
using aegis::participant::oms::RiskDecision;
using aegis::participant::oms::RiskGate;
using aegis::participant::oms::RiskVerdict;
using aegis::participant::oms::to_tracked_orders;
using aegis::participant::oms::write_oms_snapshot;
using aegis::participant::portfolio::capture_portfolio_snapshot;
using aegis::participant::portfolio::Portfolio;
using aegis::participant::portfolio::PortfolioPositionRecord;
using aegis::participant::portfolio::PortfolioSnapshot;
using aegis::participant::portfolio::PortfolioSnapshotError;
using aegis::participant::portfolio::Position;
using aegis::participant::portfolio::read_portfolio_snapshot;
using aegis::participant::portfolio::restore_portfolio;
using aegis::participant::portfolio::write_portfolio_snapshot;

class AlwaysApproveRiskGate final : public RiskGate {
 public:
  [[nodiscard]] RiskDecision decide(
      const aegis::events::exchange::NewOrderCommand& /*command*/) const override {
    return RiskDecision{
        .verdict = RiskVerdict::kApprove, .resized_quantity_units = 0, .reason = ""};
  }
};

/// Builds an OrderManager with three orders in distinct, non-trivial
/// states: fully acknowledged and live, partially filled, and cancel-
/// pending -- exactly the kind of state a weak restoration would flatten
/// back to something simpler.
OrderManager make_nontrivial_manager(RecordedResponseAdapter& adapter,
                                     AlwaysApproveRiskGate& risk) {
  OrderManager manager(adapter, risk);

  const auto id1 = manager.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 50);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 701, .participant_id = 100, .client_order_id = id1});

  const auto id2 = manager.submit_new_order(1, 100, Side::kSell, OrderType::kLimit, 1100, 80);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 702, .participant_id = 100, .client_order_id = id2});
  manager.handle_trade(aegis::events::exchange::TradeEvent{.price_units = 1100,
                                                           .quantity_units = 30,
                                                           .maker_order_id = 702,
                                                           .taker_order_id = 999,
                                                           .taker_side = Side::kBuy});

  const auto id3 = manager.submit_new_order(2, 100, Side::kBuy, OrderType::kLimit, 500, 20);
  manager.handle_order_accepted(
      OrderAcceptedEvent{.order_id = 703, .participant_id = 100, .client_order_id = id3});
  EXPECT_TRUE(manager.cancel_order(id3));

  return manager;
}

// --------------------------------------------------------------- OMS codec

TEST(OmsSnapshot, RoundTripsThroughWriteAndRead) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  const OrderManager manager = make_nontrivial_manager(adapter, risk);

  const OmsSnapshot captured = capture_oms_snapshot(manager);
  const auto bytes = write_oms_snapshot(captured);
  const auto read = read_oms_snapshot(bytes);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read.value(), captured);
}

TEST(OmsSnapshot, SameStateProducesByteIdenticalOutputAcrossRepeatedCaptures) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  const OrderManager manager = make_nontrivial_manager(adapter, risk);

  const auto first = write_oms_snapshot(capture_oms_snapshot(manager));
  const auto second = write_oms_snapshot(capture_oms_snapshot(manager));
  EXPECT_EQ(first, second);
}

TEST(OmsSnapshot, UnknownVersionIsRejected) {
  OmsSnapshot snapshot;
  snapshot.snapshot_version = 999;
  const auto read = read_oms_snapshot(write_oms_snapshot(snapshot));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), OmsSnapshotError::kUnknownVersion);
}

TEST(OmsSnapshot, TruncatedPayloadIsRejected) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  const OrderManager manager = make_nontrivial_manager(adapter, risk);
  auto bytes = write_oms_snapshot(capture_oms_snapshot(manager));
  ASSERT_FALSE(bytes.empty());
  bytes.pop_back();  // Cut mid-record: no longer decodes cleanly.

  const auto read = read_oms_snapshot(bytes);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), OmsSnapshotError::kTruncated);
}

TEST(OmsSnapshot, TrailingGarbageIsRejected) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  const OrderManager manager = make_nontrivial_manager(adapter, risk);
  auto bytes = write_oms_snapshot(capture_oms_snapshot(manager));
  bytes.push_back(std::byte{0xFF});  // Extra byte the reader must not silently ignore.

  const auto read = read_oms_snapshot(bytes);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), OmsSnapshotError::kTruncated);
}

TEST(OmsSnapshot, CounterInconsistentNextClientOrderIdIsRejected) {
  OmsSnapshot snapshot;
  snapshot.next_client_order_id = 3;
  snapshot.orders.push_back(
      OmsOrderRecord{.client_order_id = 5});  // >= next_client_order_id: bogus.
  const auto read = read_oms_snapshot(write_oms_snapshot(snapshot));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), OmsSnapshotError::kCounterInconsistent);
}

// AEGIS-237: OMS lifecycle state survives restore, and restoration does not
// revert an order to a simpler state.
TEST(OmsSnapshot, RestoredOrderManagerPreservesLifecycleStateAndIndexes) {
  RecordedResponseAdapter capture_adapter({});
  AlwaysApproveRiskGate risk;
  const OrderManager original = make_nontrivial_manager(capture_adapter, risk);
  const OmsSnapshot snapshot = capture_oms_snapshot(original);

  RecordedResponseAdapter restore_adapter({});
  OrderManager restored(restore_adapter, risk, to_tracked_orders(snapshot),
                        snapshot.next_client_order_id);

  const auto original_orders = original.all_tracked_orders();
  const auto restored_orders = restored.all_tracked_orders();
  ASSERT_EQ(original_orders.size(), restored_orders.size());
  EXPECT_EQ(original_orders, restored_orders);

  // Order 1 was kAcknowledged: still not terminal, index lookup by exchange
  // id still resolves.
  const auto* restored_by_exchange_id = restored.find_by_exchange_order_id(701);
  ASSERT_NE(restored_by_exchange_id, nullptr);
  EXPECT_EQ(restored_by_exchange_id->lifecycle.state(), OrderState::kAcknowledged);

  // Order 2 was kPartiallyFilled -- restoration must not flatten it back to
  // kAcknowledged. A legal next transition (termination) still succeeds.
  const auto* restored_partial = restored.find_by_client_order_id(2);
  ASSERT_NE(restored_partial, nullptr);
  EXPECT_EQ(restored_partial->lifecycle.state(), OrderState::kPartiallyFilled);
  EXPECT_EQ(restored_partial->cumulative_filled_units, 30);
  EXPECT_EQ(restored_partial->remaining_units, 50);
  restored.handle_order_terminated(
      OrderTerminatedEvent{.order_id = 702, .reason = TerminationReason::kFilled});
  EXPECT_EQ(restored.find_by_client_order_id(2)->lifecycle.state(), OrderState::kFilled);

  // Order 3 was kCancelPending. An illegal transition must remain illegal
  // after restore too (kCancelPending -> kSubmitted is not a legal edge).
  const auto* restored_cancel_pending = restored.find_by_client_order_id(3);
  ASSERT_NE(restored_cancel_pending, nullptr);
  EXPECT_EQ(restored_cancel_pending->lifecycle.state(), OrderState::kCancelPending);

  // next_client_order_id continues where the snapshot left off: no
  // collision with any restored order's own id.
  const auto new_id = restored.submit_new_order(1, 100, Side::kBuy, OrderType::kLimit, 1000, 10);
  EXPECT_EQ(new_id, snapshot.next_client_order_id);
  EXPECT_GT(new_id, 3U);
}

// --------------------------------------------------------- Portfolio codec

Portfolio make_nontrivial_portfolio() {
  Portfolio ledger;
  ledger.apply_fill(1, Side::kBuy, 1000, 40, /*fee_units=*/4);
  ledger.apply_fill(2, Side::kSell, 2000, 15, /*fee_units=*/2);
  ledger.apply_fill(1, Side::kSell, 1050, 10, /*fee_units=*/1);  // Partial close: realizes P&L.
  return ledger;
}

TEST(PortfolioSnapshot, RoundTripsThroughWriteAndRead) {
  const Portfolio ledger = make_nontrivial_portfolio();
  const PortfolioSnapshot captured = capture_portfolio_snapshot(ledger);
  const auto read = read_portfolio_snapshot(write_portfolio_snapshot(captured));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read.value(), captured);
}

TEST(PortfolioSnapshot, SameStateProducesByteIdenticalOutputAcrossRepeatedCaptures) {
  const Portfolio ledger = make_nontrivial_portfolio();
  const auto first = write_portfolio_snapshot(capture_portfolio_snapshot(ledger));
  const auto second = write_portfolio_snapshot(capture_portfolio_snapshot(ledger));
  EXPECT_EQ(first, second);
}

TEST(PortfolioSnapshot, UnknownVersionIsRejected) {
  PortfolioSnapshot snapshot;
  snapshot.snapshot_version = 999;
  const auto read = read_portfolio_snapshot(write_portfolio_snapshot(snapshot));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), PortfolioSnapshotError::kUnknownVersion);
}

TEST(PortfolioSnapshot, TruncatedPayloadIsRejected) {
  const Portfolio ledger = make_nontrivial_portfolio();
  auto bytes = write_portfolio_snapshot(capture_portfolio_snapshot(ledger));
  ASSERT_FALSE(bytes.empty());
  bytes.resize(bytes.size() - 3);

  const auto read = read_portfolio_snapshot(bytes);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), PortfolioSnapshotError::kTruncated);
}

TEST(PortfolioSnapshot, DuplicateInstrumentIsRejected) {
  PortfolioSnapshot snapshot;
  snapshot.positions.push_back(PortfolioPositionRecord{.instrument_id = 1, .quantity_units = 10});
  snapshot.positions.push_back(
      PortfolioPositionRecord{.instrument_id = 1, .quantity_units = 20});  // Same instrument twice.
  const auto read = read_portfolio_snapshot(write_portfolio_snapshot(snapshot));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), PortfolioSnapshotError::kDuplicateInstrument);
}

// AEGIS-237/AEGIS-118: portfolio accounting survives restore, and the next
// fill after restore accounts exactly as an uninterrupted run would.
TEST(PortfolioSnapshot, RestoredPortfolioContinuesAccountingIdenticallyToUninterrupted) {
  const Portfolio original = make_nontrivial_portfolio();
  const PortfolioSnapshot snapshot = capture_portfolio_snapshot(original);
  Portfolio restored = restore_portfolio(snapshot);

  EXPECT_EQ(restored.cash_units(), original.cash_units());
  EXPECT_EQ(restored.position(1), original.position(1));
  EXPECT_EQ(restored.position(2), original.position(2));

  // Apply the same next fill to both an uninterrupted copy and the
  // restored copy; the resulting state must match exactly.
  Portfolio uninterrupted = make_nontrivial_portfolio();
  uninterrupted.apply_fill(1, Side::kBuy, 1020, 5, /*fee_units=*/1);
  restored.apply_fill(1, Side::kBuy, 1020, 5, /*fee_units=*/1);

  EXPECT_EQ(restored.cash_units(), uninterrupted.cash_units());
  EXPECT_EQ(restored.position(1), uninterrupted.position(1));
  EXPECT_EQ(restored.position(2), uninterrupted.position(2));
}

// ----------------------------------------------------- Outer app composition

TEST(ParticipantSnapshot, RoundTripsThroughWriteAndRead) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  const OrderManager manager = make_nontrivial_manager(adapter, risk);
  const Portfolio ledger = make_nontrivial_portfolio();

  const ParticipantSnapshot captured = capture_participant_snapshot(manager, ledger);
  const auto read = read_participant_snapshot(write_participant_snapshot(captured));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read.value(), captured);
}

TEST(ParticipantSnapshot, SameStateProducesByteIdenticalOutputAcrossRepeatedCaptures) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  const OrderManager manager = make_nontrivial_manager(adapter, risk);
  const Portfolio ledger = make_nontrivial_portfolio();

  const auto first = write_participant_snapshot(capture_participant_snapshot(manager, ledger));
  const auto second = write_participant_snapshot(capture_participant_snapshot(manager, ledger));
  EXPECT_EQ(first, second);
}

TEST(ParticipantSnapshot, UnknownOuterVersionIsRejected) {
  ParticipantSnapshot snapshot;
  snapshot.snapshot_version = 999;
  const auto read = read_participant_snapshot(write_participant_snapshot(snapshot));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), ParticipantSnapshotError::kUnknownVersion);
}

TEST(ParticipantSnapshot, InvalidComponentIsRejected) {
  ParticipantSnapshot snapshot;
  snapshot.oms.snapshot_version = 999;  // Corrupt only the inner OMS component.
  const auto read = read_participant_snapshot(write_participant_snapshot(snapshot));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), ParticipantSnapshotError::kComponentInvalid);
}

TEST(ParticipantSnapshot, TruncatedPayloadIsRejected) {
  RecordedResponseAdapter adapter({});
  AlwaysApproveRiskGate risk;
  const OrderManager manager = make_nontrivial_manager(adapter, risk);
  const Portfolio ledger = make_nontrivial_portfolio();
  auto bytes = write_participant_snapshot(capture_participant_snapshot(manager, ledger));
  ASSERT_GT(bytes.size(), 5U);
  bytes.resize(bytes.size() - 5);

  const auto read = read_participant_snapshot(bytes);
  ASSERT_FALSE(read.has_value());
}

}  // namespace
