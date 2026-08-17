#include <array>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/common/clock.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/exchange/app/exchange_node.hpp"
#include "cpp/participant/book_builder/book_builder.hpp"
#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/oms/transport_execution_adapter.hpp"
#include "cpp/participant/portfolio/portfolio.hpp"
#include "cpp/participant/strategy/calendar_spread_strategy.hpp"
#include "tests/cpp/optional_access.hpp"
#include "tests/cpp/support/in_process_exchange_transport.hpp"

/// AEGIS-004, AEGIS-076..081; ADR-0025: the M4 calendar-spread strategy
/// against a real, unmodified M1 `ExchangeNode` and real FIFO matching --
/// Demo B of the first real-deal path
/// (`cpp/participant/app/participant_run.hpp`'s `run_calendar_spread_scenario`
/// is Demo A, the production composition, which has no exchange dependency
/// at all). Composed entirely inside `tests/`, outside `covered_roots`
/// (`configs/architecture_rules.yaml`): no production
/// `cpp-participant-strategy -> cpp-exchange-*` edge exists anywhere, exactly
/// as `test_participant_exchange_integration.cpp` already establishes for
/// the OMS. Reuses that file's `InProcessExchangeTransport`
/// (`tests/cpp/support/in_process_exchange_transport.hpp`) rather than a
/// second copy of the same test-only transport.
///
/// The chain under test: reconstructed near/far `TopOfBook` ->
/// `CalendarSpreadStrategy::on_book_update` -> a `StrategyProposal` ->
/// the existing always-approve risk-gate double (ADR-0023: no production
/// `RiskGate` ships before M5) -> `OrderManager` -> `TransportExecutionAdapter`
/// -> `InProcessExchangeTransport` -> a real `ExchangeNode` -> real FIFO
/// matching against pre-seeded resting counterparty liquidity -> the
/// resulting `EmittedEvent`s decoded back into `OrderManager` and
/// `Portfolio`.
namespace {

using aegis::common::ManualClock;
using aegis::events::Envelope;
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
using aegis::participant::book::BookBuilder;
using aegis::participant::oms::OrderManager;
using aegis::participant::oms::OrderState;
using aegis::participant::oms::RiskDecision;
using aegis::participant::oms::RiskGate;
using aegis::participant::oms::RiskVerdict;
using aegis::participant::oms::TransportExecutionAdapter;
using aegis::participant::portfolio::Portfolio;
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
  // price_floor_units must itself be tick-aligned with every price this test
  // uses: validate_price checks (price - floor) % tick_size == 0, not price %
  // tick_size == 0, so a floor of 0 -- not 1 -- is what makes 100'000/100'060
  // legal ticks at tick_size_units = 5.
  spec.price_floor_units = PriceUnits{0};
  spec.price_ceiling_units = PriceUnits{1'000'000};
  spec.tick_size_units = 5;
  spec.min_quantity_units = QuantityUnits{25};
  spec.max_quantity_units = QuantityUnits{100'000};
  spec.lot_size_units = 25;
  return spec;
}

class AlwaysApproveRiskGate final : public RiskGate {
 public:
  [[nodiscard]] RiskDecision decide(const NewOrderCommand& /*command*/) const override {
    return RiskDecision{
        .verdict = RiskVerdict::kApprove, .resized_quantity_units = 0, .reason = ""};
  }
};

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

/// Submits `leg` as a market order through `manager`, feeds every resulting
/// `EmittedEvent` back into `manager`/`portfolio` exactly as a production
/// feed handler decoding the wire would (mirroring
/// `test_participant_exchange_integration.cpp`'s `deliver()`, generalized to
/// either side).
void execute_leg_against_real_exchange(const StrategyLeg& leg, OrderManager& manager,
                                       Portfolio& portfolio, InProcessExchangeTransport& transport) {
  const auto client_order_id = manager.submit_new_order(
      leg.instrument_id, /*participant_id=*/1, leg.side, OrderType::kMarket,
      /*price_units=*/0, leg.quantity_units);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_NE(tracked, nullptr);

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
          if (decoded->maker_order_id == our_order_id) {
            const Side maker_side =
                decoded->taker_side == Side::kBuy ? Side::kSell : Side::kBuy;
            portfolio.apply_fill(leg.instrument_id, maker_side, decoded->price_units,
                                 decoded->quantity_units);
          } else if (decoded->taker_order_id == our_order_id) {
            portfolio.apply_fill(leg.instrument_id, decoded->taker_side, decoded->price_units,
                                 decoded->quantity_units);
          }
        }
        break;
      case MessageType::kOrderTerminated:
        if (const auto decoded = decode_order_terminated(event.payload); decoded.has_value()) {
          manager.handle_order_terminated(*decoded);
        }
        break;
      default:
        break;
    }
  }
}

/// Runs the full scenario once: three near/far book updates (the same
/// leading three spread observations `test_calendar_spread_strategy.cpp`
/// verifies land an entry on the third), submitting the resulting proposal's
/// two legs through the real exchange. Returns the final tracked-order
/// states and portfolio for the caller to assert on, and to compare across
/// two independent runs for the determinism proof.
struct ScenarioResult {
  OrderState near_state{OrderState::kCreated};
  OrderState far_state{OrderState::kCreated};
  std::int64_t near_position_units{0};
  std::int64_t far_position_units{0};
  std::int64_t cash_units{0};
};

ScenarioResult run_scenario() {
  ExchangeNode node;
  node.add_instrument(make_spec(kNearInstrumentId));
  node.add_instrument(make_spec(kFarInstrumentId));
  ManualClock clock;

  // Resting counterparty liquidity for both legs of the spread this exact
  // sequence is known to enter (see test_calendar_spread_strategy.cpp for
  // the identical z-score arithmetic): a short spread sells near, buys far.
  const auto near_counterparty_events = node.apply_new_order(
      NewOrderCommand{.instrument_id = kNearInstrumentId,
                     .participant_id = 2,
                     .client_order_id = 1,
                     .side = Side::kBuy,
                     .order_type = OrderType::kLimit,
                     .price_units = 100'000,
                     .quantity_units = kQuantityUnits},
      node.sequencer().sequence(clock.stamp<aegis::common::EventTime>()));
  const auto far_counterparty_events = node.apply_new_order(
      NewOrderCommand{.instrument_id = kFarInstrumentId,
                     .participant_id = 3,
                     .client_order_id = 1,
                     .side = Side::kSell,
                     .order_type = OrderType::kLimit,
                     .price_units = 100'060,
                     .quantity_units = kQuantityUnits},
      node.sequencer().sequence(clock.stamp<aegis::common::EventTime>()));
  EXPECT_EQ(near_counterparty_events.size(), 1U);
  EXPECT_EQ(near_counterparty_events.front().message_type, MessageType::kOrderAccepted);
  EXPECT_EQ(far_counterparty_events.size(), 1U);
  EXPECT_EQ(far_counterparty_events.front().message_type, MessageType::kOrderAccepted);

  InProcessExchangeTransport transport(node, clock);
  TransportExecutionAdapter adapter(transport, clock, /*stream_id=*/1);
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  Portfolio portfolio;

  // 1. Near/far market state, reconstructed through real BookBuilder
  // instances -- the same participant-side machinery
  // cpp/participant/app/participant_run.cpp composes in production.
  BookBuilder near_book(kNearInstrumentId);
  BookBuilder far_book(kFarInstrumentId);
  const CalendarSpreadConfig config{.near_instrument_id = kNearInstrumentId,
                                    .far_instrument_id = kFarInstrumentId,
                                    .zscore_window = 20,
                                    .entry_threshold = 2.0,
                                    .exit_threshold = 0.5,
                                    .quantity_units = kQuantityUnits};
  CalendarSpreadStrategy strategy(config);

  // Spread (far mid - near mid), in price_units: 50, 55, 60 -- the leading
  // three values of test_calendar_spread_strategy.cpp's own sequence.
  // z-scores are invariant to a uniform scale (these are the same
  // 0.50/0.55/0.60 dollars, times 100), so the third update reproduces that
  // test's z ~= 2.1213 >= entry_threshold exactly, landing a short-spread
  // entry: near sells, far buys.
  constexpr std::int64_t kNearMid = 100'000;
  const std::array<std::int64_t, 3> far_bases{100'050, 100'055, 100'060};

  StrategyProposal proposal;
  for (std::size_t i = 0; i < far_bases.size(); ++i) {
    near_book.apply_snapshot(make_snapshot(kNearInstrumentId, i + 1, kNearMid, /*half_spread=*/10));
    far_book.apply_snapshot(
        make_snapshot(kFarInstrumentId, i + 1, far_bases.at(i), /*half_spread=*/10));
    proposal = strategy.on_book_update(near_book.top_of_book(), far_book.top_of_book());
  }

  // 2. A non-trivial proposal from that reconstructed state.
  EXPECT_TRUE(proposal.has_action);
  EXPECT_EQ(strategy.position(), SpreadPosition::kShortSpread);
  EXPECT_EQ(proposal.near.side, Side::kSell);
  EXPECT_EQ(proposal.far.side, Side::kBuy);

  // 3-9: risk seam -> OMS -> real M1 matching -> fills -> lifecycle ->
  // portfolio, for both legs.
  execute_leg_against_real_exchange(proposal.near, manager, portfolio, transport);
  execute_leg_against_real_exchange(proposal.far, manager, portfolio, transport);

  ScenarioResult result;
  const auto tracked_orders = manager.all_tracked_orders();
  EXPECT_EQ(tracked_orders.size(), 2U);
  for (const auto& tracked : tracked_orders) {
    if (tracked.instrument_id == kNearInstrumentId) {
      result.near_state = tracked.lifecycle.state();
    } else {
      result.far_state = tracked.lifecycle.state();
    }
  }
  result.near_position_units = portfolio.position(kNearInstrumentId).quantity_units;
  result.far_position_units = portfolio.position(kFarInstrumentId).quantity_units;
  result.cash_units = portfolio.cash_units();
  return result;
}

TEST(CalendarSpreadExchangeIntegration,
    StrategyProposalReachesRealFifoMatchingAndUpdatesPortfolio) {
  const ScenarioResult result = run_scenario();

  // 6-8: real FIFO matching filled both legs completely (kQuantityUnits ==
  // the resting counterparty's full size on each side), OMS lifecycle
  // advanced to kFilled -- never inferred, only reached via the exchange's
  // own OrderTerminatedEvent{kFilled} (ADR-0011).
  EXPECT_EQ(result.near_state, OrderState::kFilled);
  EXPECT_EQ(result.far_state, OrderState::kFilled);

  // 9: portfolio position/cash updated -- short the near leg, long the far
  // leg, both at the fixed size.
  EXPECT_EQ(result.near_position_units, -kQuantityUnits);
  EXPECT_EQ(result.far_position_units, kQuantityUnits);

  // 10: a non-trivial accounting effect. Selling near at 100'000 and buying
  // far at 100'060 for the same size is a real cash flow, not a wash.
  EXPECT_NE(result.cash_units, 0);
}

// 11: two independent runs of the same deterministic scenario -- fresh
// ExchangeNode, fresh clock, fresh strategy/OMS/portfolio state each time --
// produce identical canonical output.
TEST(CalendarSpreadExchangeIntegration, RepeatedRunIsDeterministic) {
  const ScenarioResult first = run_scenario();
  const ScenarioResult second = run_scenario();

  EXPECT_EQ(first.near_state, second.near_state);
  EXPECT_EQ(first.far_state, second.far_state);
  EXPECT_EQ(first.near_position_units, second.near_position_units);
  EXPECT_EQ(first.far_position_units, second.far_position_units);
  EXPECT_EQ(first.cash_units, second.cash_units);
}

}  // namespace
