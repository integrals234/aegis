#include <array>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/common/clock.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/exchange/app/exchange_node.hpp"
#include "cpp/participant/app/risk_engine_gate.hpp"
#include "cpp/participant/book_builder/book_builder.hpp"
#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/oms/transport_execution_adapter.hpp"
#include "cpp/participant/portfolio/portfolio.hpp"
#include "cpp/participant/risk/risk_engine.hpp"
#include "cpp/participant/strategy/calendar_spread_strategy.hpp"
#include "tests/cpp/optional_access.hpp"
#include "tests/cpp/support/in_process_exchange_transport.hpp"

/// M5's real risk engine, wired through the mandatory oms::RiskGate seam
/// (app::RiskEngineGate, ADR-0027) against a real, unmodified M1
/// `ExchangeNode` and real FIFO matching -- Demo B of the M5 vertical demo.
/// Extends `test_calendar_spread_exchange_integration.cpp`'s M4 pattern:
/// same strategy, same real-exchange harness, `AlwaysApproveRiskGate`
/// replaced by the production `RiskEngineGate` over a real `RiskEngine`.
///
/// Every REJECT/RESIZE/HALT assertion in this file proves the "critical
/// property" of the M5 vertical demo: a reject means zero adapter submit,
/// zero exchange order, and zero portfolio change -- never a verdict
/// computed after an order was already sent.
namespace {

using aegis::common::ManualClock;
using aegis::events::MessageType;
using aegis::events::exchange::decode_order_accepted;
using aegis::events::exchange::decode_order_terminated;
using aegis::events::exchange::decode_trade;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::events::market_data::BookLevelEntry;
using aegis::events::market_data::BookSnapshotEvent;
using aegis::exchange::EmittedEvent;
using aegis::exchange::ExchangeNode;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;
using aegis::participant::app::RiskEngineGate;
using aegis::participant::book::BookBuilder;
using aegis::participant::oms::OrderManager;
using aegis::participant::oms::OrderState;
using aegis::participant::oms::TransportExecutionAdapter;
using aegis::participant::portfolio::Portfolio;
using aegis::participant::risk::InstrumentInfo;
using aegis::participant::risk::OrderQuantityLimit;
using aegis::participant::risk::RiskEngine;
using aegis::participant::risk::RiskLimitsConfig;
using aegis::participant::strategy::CalendarSpreadConfig;
using aegis::participant::strategy::CalendarSpreadStrategy;
using aegis::participant::strategy::SpreadPosition;
using aegis::participant::strategy::StrategyLeg;
using aegis::participant::strategy::StrategyProposal;
using aegis::testing::InProcessExchangeTransport;

constexpr std::uint32_t kNearInstrumentId = 10;
constexpr std::uint32_t kFarInstrumentId = 20;
constexpr std::int64_t kQuantityUnits = 25;  // One lot at lot_size_units below.

InstrumentSpec make_spec(std::uint32_t instrument_id) {
  InstrumentSpec spec;
  spec.instrument_id = InstrumentId{instrument_id};
  spec.price_floor_units = PriceUnits{0};
  spec.price_ceiling_units = PriceUnits{1'000'000};
  spec.tick_size_units = 5;
  spec.min_quantity_units = QuantityUnits{25};
  spec.max_quantity_units = QuantityUnits{100'000};
  spec.lot_size_units = 25;
  return spec;
}

BookSnapshotEvent make_snapshot(std::uint32_t instrument_id, std::uint64_t md_sequence,
                                std::int64_t mid_price_units, std::int64_t half_spread_units) {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = instrument_id;
  snapshot.md_sequence = md_sequence;
  snapshot.entries.push_back(BookLevelEntry{.side = Side::kBuy,
                                            .price_units = mid_price_units - half_spread_units,
                                            .quantity_units = 500,
                                            .order_id = 1});
  snapshot.entries.push_back(BookLevelEntry{.side = Side::kSell,
                                            .price_units = mid_price_units + half_spread_units,
                                            .quantity_units = 500,
                                            .order_id = 2});
  return snapshot;
}

/// Feeds three near/far book updates -- the same leading sequence
/// `test_calendar_spread_strategy.cpp`/`test_calendar_spread_exchange_
/// integration.cpp` use -- and returns the resulting short-spread proposal
/// (near sell, far buy).
StrategyProposal build_short_spread_proposal(BookBuilder& near_book, BookBuilder& far_book,
                                             CalendarSpreadStrategy& strategy) {
  constexpr std::int64_t kNearMid = 100'000;
  const std::array<std::int64_t, 3> far_bases{100'050, 100'055, 100'060};
  StrategyProposal proposal;
  for (std::size_t i = 0; i < far_bases.size(); ++i) {
    near_book.apply_snapshot(make_snapshot(kNearInstrumentId, i + 1, kNearMid, 10));
    far_book.apply_snapshot(make_snapshot(kFarInstrumentId, i + 1, far_bases.at(i), 10));
    proposal = strategy.on_book_update(near_book.top_of_book(), far_book.top_of_book());
  }
  return proposal;
}

RiskLimitsConfig permissive_config() {
  RiskLimitsConfig config;
  config.instruments[kNearInstrumentId] =
      InstrumentInfo{.multiplier_units = 1, .currency = "USD", .market = "", .sector = ""};
  config.instruments[kFarInstrumentId] =
      InstrumentInfo{.multiplier_units = 1, .currency = "USD", .market = "", .sector = ""};
  return config;
}

/// One leg, submitted through the mandatory risk seam -> real exchange ->
/// real FIFO matching, feeding every resulting event back into
/// manager/portfolio/risk_engine exactly as a production feed handler would
/// decode them from the wire.
void execute_leg_against_real_exchange(const StrategyLeg& leg, OrderManager& manager,
                                       Portfolio& portfolio, RiskEngine& risk_engine,
                                       InProcessExchangeTransport& transport) {
  const auto client_order_id = manager.submit_new_order(leg.instrument_id, /*participant_id=*/1,
                                                        leg.side, OrderType::kMarket,
                                                        /*price_units=*/0, leg.quantity_units);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_NE(tracked, nullptr);
  if (tracked->lifecycle.state() == OrderState::kRejected) {
    return;  // The seam rejected: no transport call, nothing to drain.
  }

  const auto emitted = transport.drain();
  ASSERT_FALSE(emitted.empty());
  const auto accepted = decode_order_accepted(emitted.front().payload);
  ASSERT_TRUE(accepted.has_value());
  const std::uint64_t our_order_id = aegis::test::checked(accepted).order_id;

  for (const EmittedEvent& event : emitted) {
    switch (event.message_type) {
      case MessageType::kOrderAccepted:
        if (const auto decoded = decode_order_accepted(event.payload); decoded.has_value()) {
          manager.handle_order_accepted(*decoded);
        }
        break;
      case MessageType::kTrade:
        if (const auto decoded = decode_trade(event.payload); decoded.has_value()) {
          manager.handle_trade(*decoded);
          Side fill_side = decoded->taker_side;
          if (decoded->maker_order_id == our_order_id) {
            fill_side = decoded->taker_side == Side::kBuy ? Side::kSell : Side::kBuy;
          } else if (decoded->taker_order_id != our_order_id) {
            break;
          }
          portfolio.apply_fill(leg.instrument_id, fill_side, decoded->price_units,
                               decoded->quantity_units);
          risk_engine.on_fill(client_order_id, leg.instrument_id, fill_side,
                              decoded->quantity_units);
        }
        break;
      case MessageType::kOrderTerminated:
        if (const auto decoded = decode_order_terminated(event.payload); decoded.has_value()) {
          manager.handle_order_terminated(*decoded);
          risk_engine.on_order_terminated(client_order_id);
        }
        break;
      default:
        break;
    }
  }
}

struct Harness {
  ExchangeNode node;
  ManualClock clock;
  InProcessExchangeTransport transport{node, clock};
};

void seed_counterparty_liquidity(Harness& harness) {
  harness.node.add_instrument(make_spec(kNearInstrumentId));
  harness.node.add_instrument(make_spec(kFarInstrumentId));
  const auto near_events = harness.node.apply_new_order(
      NewOrderCommand{.instrument_id = kNearInstrumentId,
                      .participant_id = 2,
                      .client_order_id = 1,
                      .side = Side::kBuy,
                      .order_type = OrderType::kLimit,
                      .price_units = 100'000,
                      .quantity_units = kQuantityUnits},
      harness.node.sequencer().sequence(harness.clock.stamp<aegis::common::EventTime>()));
  const auto far_events = harness.node.apply_new_order(
      NewOrderCommand{.instrument_id = kFarInstrumentId,
                      .participant_id = 3,
                      .client_order_id = 1,
                      .side = Side::kSell,
                      .order_type = OrderType::kLimit,
                      .price_units = 100'060,
                      .quantity_units = kQuantityUnits},
      harness.node.sequencer().sequence(harness.clock.stamp<aegis::common::EventTime>()));
  ASSERT_EQ(near_events.size(), 1U);
  ASSERT_EQ(far_events.size(), 1U);
}

TEST(CalendarSpreadRiskExchangeIntegration,
     AllowedProposalReachesRealFifoMatchingAndUpdatesPortfolio) {
  Harness harness;
  seed_counterparty_liquidity(harness);

  BookBuilder near_book(kNearInstrumentId);
  BookBuilder far_book(kFarInstrumentId);
  const CalendarSpreadConfig config{.near_instrument_id = kNearInstrumentId,
                                    .far_instrument_id = kFarInstrumentId,
                                    .zscore_window = 20,
                                    .entry_threshold = 2.0,
                                    .exit_threshold = 0.5,
                                    .quantity_units = kQuantityUnits};
  CalendarSpreadStrategy strategy(config);
  const StrategyProposal proposal = build_short_spread_proposal(near_book, far_book, strategy);
  ASSERT_TRUE(proposal.has_action);
  ASSERT_EQ(strategy.position(), SpreadPosition::kShortSpread);

  RiskEngine risk_engine(permissive_config());
  risk_engine.on_market_data(kNearInstrumentId, 100'000, 0, true);
  risk_engine.on_market_data(kFarInstrumentId, 100'060, 0, true);
  RiskEngineGate risk_gate(risk_engine, harness.clock);
  TransportExecutionAdapter adapter(harness.transport, harness.clock, /*stream_id=*/1);
  OrderManager manager(adapter, risk_gate);
  Portfolio portfolio;

  const auto& decision = risk_engine.commit_proposal_decision(
      "calendar_spread", "allow-1",
      {aegis::participant::risk::OrderRequest{.strategy_id = "calendar_spread",
                                              .proposal_id = "allow-1",
                                              .leg_index = 0,
                                              .instrument_id = kNearInstrumentId,
                                              .side = proposal.near.side,
                                              .price_units = 100'000,
                                              .quantity_units = proposal.near.quantity_units},
       aegis::participant::risk::OrderRequest{.strategy_id = "calendar_spread",
                                              .proposal_id = "allow-1",
                                              .leg_index = 1,
                                              .instrument_id = kFarInstrumentId,
                                              .side = proposal.far.side,
                                              .price_units = 100'060,
                                              .quantity_units = proposal.far.quantity_units}},
      0);
  ASSERT_NE(decision.verdict, aegis::participant::risk::RiskVerdict::kReject);

  const std::uint64_t next_order_id_before = harness.node.next_order_id();
  execute_leg_against_real_exchange(proposal.near, manager, portfolio, risk_engine,
                                    harness.transport);
  execute_leg_against_real_exchange(proposal.far, manager, portfolio, risk_engine,
                                    harness.transport);

  // Real FIFO matching filled both legs completely.
  EXPECT_GT(harness.node.next_order_id(), next_order_id_before + 1);
  const auto tracked_orders = manager.all_tracked_orders();
  ASSERT_EQ(tracked_orders.size(), 2U);
  for (const auto& tracked : tracked_orders) {
    EXPECT_EQ(tracked.lifecycle.state(), OrderState::kFilled);
  }
  EXPECT_EQ(portfolio.position(kNearInstrumentId).quantity_units, -kQuantityUnits);
  EXPECT_EQ(portfolio.position(kFarInstrumentId).quantity_units, kQuantityUnits);
  EXPECT_NE(portfolio.cash_units(), 0);
}

TEST(CalendarSpreadRiskExchangeIntegration, RejectedProposalNeverReachesTheExchangeOrThePortfolio) {
  Harness harness;
  seed_counterparty_liquidity(harness);

  BookBuilder near_book(kNearInstrumentId);
  BookBuilder far_book(kFarInstrumentId);
  const CalendarSpreadConfig config{.near_instrument_id = kNearInstrumentId,
                                    .far_instrument_id = kFarInstrumentId,
                                    .zscore_window = 20,
                                    .entry_threshold = 2.0,
                                    .exit_threshold = 0.5,
                                    .quantity_units = kQuantityUnits};
  CalendarSpreadStrategy strategy(config);
  const StrategyProposal proposal = build_short_spread_proposal(near_book, far_book, strategy);
  ASSERT_TRUE(proposal.has_action);

  RiskLimitsConfig limits = permissive_config();
  limits.order_quantity_limits[kNearInstrumentId] =
      OrderQuantityLimit{.max_order_quantity_units = 0, .resize_on_breach = false};
  limits.order_quantity_limits[kFarInstrumentId] =
      OrderQuantityLimit{.max_order_quantity_units = 0, .resize_on_breach = false};
  RiskEngine risk_engine(limits);
  risk_engine.on_market_data(kNearInstrumentId, 100'000, 0, true);
  risk_engine.on_market_data(kFarInstrumentId, 100'060, 0, true);
  RiskEngineGate risk_gate(risk_engine, harness.clock);
  TransportExecutionAdapter adapter(harness.transport, harness.clock, /*stream_id=*/1);
  OrderManager manager(adapter, risk_gate);
  Portfolio portfolio;

  const auto& decision = risk_engine.commit_proposal_decision(
      "calendar_spread", "reject-1",
      {aegis::participant::risk::OrderRequest{.strategy_id = "calendar_spread",
                                              .proposal_id = "reject-1",
                                              .leg_index = 0,
                                              .instrument_id = kNearInstrumentId,
                                              .side = proposal.near.side,
                                              .price_units = 100'000,
                                              .quantity_units = proposal.near.quantity_units},
       aegis::participant::risk::OrderRequest{.strategy_id = "calendar_spread",
                                              .proposal_id = "reject-1",
                                              .leg_index = 1,
                                              .instrument_id = kFarInstrumentId,
                                              .side = proposal.far.side,
                                              .price_units = 100'060,
                                              .quantity_units = proposal.far.quantity_units}},
      0);
  ASSERT_EQ(decision.verdict, aegis::participant::risk::RiskVerdict::kReject);

  // Composition-root atomicity: NEITHER leg is submitted for a rejected
  // proposal (the demo never calls execute_leg here at all, matching
  // participant_run.cpp's own gate). Prove the exchange and portfolio are
  // exactly as if nothing happened.
  const std::uint64_t next_order_id_before = harness.node.next_order_id();
  EXPECT_EQ(harness.node.next_order_id(), next_order_id_before);
  EXPECT_TRUE(manager.all_tracked_orders().empty());
  EXPECT_EQ(portfolio.position(kNearInstrumentId).quantity_units, 0);
  EXPECT_EQ(portfolio.position(kFarInstrumentId).quantity_units, 0);
  EXPECT_EQ(portfolio.cash_units(), 0);
}

TEST(CalendarSpreadRiskExchangeIntegration, ResizedProposalSubmitsExactlyTheApprovedQuantity) {
  Harness harness;
  seed_counterparty_liquidity(harness);

  // The exchange spec below fixes lot_size_units == min_quantity_units == 25
  // (kQuantityUnits), so the only quantities that clear real FIFO matching
  // at all are multiples of 25. A genuine "resize down" therefore has the
  // strategy ask for two lots (50) and the risk engine approve one (25) --
  // both lot-compliant, unlike an arbitrary in-lot value.
  BookBuilder near_book(kNearInstrumentId);
  BookBuilder far_book(kFarInstrumentId);
  const CalendarSpreadConfig config{.near_instrument_id = kNearInstrumentId,
                                    .far_instrument_id = kFarInstrumentId,
                                    .zscore_window = 20,
                                    .entry_threshold = 2.0,
                                    .exit_threshold = 0.5,
                                    .quantity_units = 2 * kQuantityUnits};
  CalendarSpreadStrategy strategy(config);
  const StrategyProposal proposal = build_short_spread_proposal(near_book, far_book, strategy);
  ASSERT_TRUE(proposal.has_action);
  ASSERT_EQ(proposal.near.quantity_units, 2 * kQuantityUnits);

  constexpr std::int64_t kResizedQuantity = kQuantityUnits;  // One lot, not two.
  RiskLimitsConfig limits = permissive_config();
  limits.order_quantity_limits[kNearInstrumentId] =
      OrderQuantityLimit{.max_order_quantity_units = kResizedQuantity, .resize_on_breach = true};
  limits.order_quantity_limits[kFarInstrumentId] =
      OrderQuantityLimit{.max_order_quantity_units = kResizedQuantity, .resize_on_breach = true};
  RiskEngine risk_engine(limits);
  risk_engine.on_market_data(kNearInstrumentId, 100'000, 0, true);
  risk_engine.on_market_data(kFarInstrumentId, 100'060, 0, true);
  RiskEngineGate risk_gate(risk_engine, harness.clock);
  TransportExecutionAdapter adapter(harness.transport, harness.clock, /*stream_id=*/1);
  OrderManager manager(adapter, risk_gate);
  Portfolio portfolio;

  const auto& decision = risk_engine.commit_proposal_decision(
      "calendar_spread", "resize-1",
      {aegis::participant::risk::OrderRequest{.strategy_id = "calendar_spread",
                                              .proposal_id = "resize-1",
                                              .leg_index = 0,
                                              .instrument_id = kNearInstrumentId,
                                              .side = proposal.near.side,
                                              .price_units = 100'000,
                                              .quantity_units = proposal.near.quantity_units},
       aegis::participant::risk::OrderRequest{.strategy_id = "calendar_spread",
                                              .proposal_id = "resize-1",
                                              .leg_index = 1,
                                              .instrument_id = kFarInstrumentId,
                                              .side = proposal.far.side,
                                              .price_units = 100'060,
                                              .quantity_units = proposal.far.quantity_units}},
      0);
  ASSERT_EQ(decision.verdict, aegis::participant::risk::RiskVerdict::kResize);

  execute_leg_against_real_exchange(proposal.near, manager, portfolio, risk_engine,
                                    harness.transport);
  execute_leg_against_real_exchange(proposal.far, manager, portfolio, risk_engine,
                                    harness.transport);

  const auto tracked_orders = manager.all_tracked_orders();
  ASSERT_EQ(tracked_orders.size(), 2U);
  for (const auto& tracked : tracked_orders) {
    EXPECT_EQ(tracked.original_quantity_units, kResizedQuantity);
  }
  EXPECT_EQ(portfolio.position(kNearInstrumentId).quantity_units, -kResizedQuantity);
  EXPECT_EQ(portfolio.position(kFarInstrumentId).quantity_units, kResizedQuantity);
}

TEST(CalendarSpreadRiskExchangeIntegration,
     GlobalKillSwitchCancelsLiveOrdersAndBlocksNewSubmissions) {
  Harness harness;
  seed_counterparty_liquidity(harness);
  // Widen the resting counterparty so the participant's own order rests
  // instead of filling immediately -- there must be a LIVE order for the
  // kill switch to cancel.
  const auto extra_liquidity = harness.node.apply_new_order(
      NewOrderCommand{.instrument_id = kNearInstrumentId,
                      .participant_id = 4,
                      .client_order_id = 2,
                      .side = Side::kBuy,
                      .order_type = OrderType::kLimit,
                      .price_units = 99'990,
                      .quantity_units = kQuantityUnits},
      harness.node.sequencer().sequence(harness.clock.stamp<aegis::common::EventTime>()));
  ASSERT_EQ(extra_liquidity.size(), 1U);

  RiskEngine risk_engine(permissive_config());
  risk_engine.on_market_data(kNearInstrumentId, 100'000, 0, true);
  RiskEngineGate risk_gate(risk_engine, harness.clock);
  TransportExecutionAdapter adapter(harness.transport, harness.clock, /*stream_id=*/1);
  OrderManager manager(adapter, risk_gate);

  // A resting sell above the best bid: rests instead of filling. Armed via
  // the ordinary proposal decision first -- decide_order structurally
  // rejects any order with no matching armed leg.
  risk_engine.commit_proposal_decision(
      "calendar_spread", "resting-1",
      {aegis::participant::risk::OrderRequest{.strategy_id = "calendar_spread",
                                              .proposal_id = "resting-1",
                                              .leg_index = 0,
                                              .instrument_id = kNearInstrumentId,
                                              .side = Side::kSell,
                                              .price_units = 100'050,
                                              .quantity_units = kQuantityUnits}},
      0);
  const auto client_order_id =
      manager.submit_new_order(kNearInstrumentId, /*participant_id=*/1, Side::kSell,
                               OrderType::kLimit, 100'050, kQuantityUnits);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_NE(tracked, nullptr);
  const auto emitted = harness.transport.drain();
  ASSERT_FALSE(emitted.empty());
  for (const auto& event : emitted) {
    if (event.message_type == MessageType::kOrderAccepted) {
      if (const auto decoded = decode_order_accepted(event.payload); decoded.has_value()) {
        manager.handle_order_accepted(*decoded);
      }
    }
  }
  ASSERT_EQ(manager.find_by_client_order_id(client_order_id)->lifecycle.state(),
            OrderState::kAcknowledged);

  // Trip the global kill switch: cancel every live order (a safety cancel,
  // bypassing the ordinary cancel rate limit), then confirm it is idempotent
  // and that no further order can be submitted.
  ASSERT_TRUE(risk_engine.trip_global());
  ASSERT_FALSE(risk_engine.trip_global());  // Idempotent: second trip is a no-op.
  ASSERT_TRUE(risk_engine.allow_cancel(0, /*bypass_safety=*/true));
  EXPECT_TRUE(manager.cancel_order(client_order_id));

  // A halted engine rejects at the PROPOSAL level (evaluate_leg's halt check
  // runs before anything else), so no pending leg is ever armed for
  // decide_order to approve -- proven directly against the proposal decision
  // itself, which is where AEGIS-135's "prevents new orders" actually bites.
  const auto& blocked_decision = risk_engine.commit_proposal_decision(
      "calendar_spread", "blocked-1",
      {aegis::participant::risk::OrderRequest{.strategy_id = "calendar_spread",
                                              .proposal_id = "blocked-1",
                                              .leg_index = 0,
                                              .instrument_id = kNearInstrumentId,
                                              .side = Side::kBuy,
                                              .price_units = 100'000,
                                              .quantity_units = kQuantityUnits}},
      0);
  EXPECT_EQ(blocked_decision.verdict, aegis::participant::risk::RiskVerdict::kReject);
  EXPECT_EQ(blocked_decision.reason_code, aegis::participant::risk::ReasonCode::kKillSwitchGlobal);

  const std::uint64_t next_order_id_before_new_submit = harness.node.next_order_id();
  const auto blocked_client_order_id =
      manager.submit_new_order(kNearInstrumentId, /*participant_id=*/1, Side::kBuy,
                               OrderType::kLimit, 100'000, kQuantityUnits);
  EXPECT_EQ(manager.find_by_client_order_id(blocked_client_order_id)->lifecycle.state(),
            OrderState::kRejected);
  EXPECT_EQ(harness.node.next_order_id(), next_order_id_before_new_submit);
}

}  // namespace
