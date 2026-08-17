#include <vector>

#include <gtest/gtest.h>

#include "cpp/common/clock.hpp"
#include "cpp/exchange/app/exchange_node.hpp"
#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/oms/transport_execution_adapter.hpp"
#include "cpp/participant/portfolio/portfolio.hpp"
#include "tests/cpp/optional_access.hpp"
#include "tests/cpp/support/in_process_exchange_transport.hpp"

/// AEGIS-109, AEGIS-110, AEGIS-111, AEGIS-114: real M1 exchange integration,
/// composed entirely inside `tests/` -- outside `covered_roots`
/// (`configs/architecture_rules.yaml`), so this file may legally see both
/// the exchange and the participant at once without creating a production
/// participant -> exchange dependency (ADR-0023, MASTER_SPEC immutable
/// principle 1). `aegis::testing::InProcessExchangeTransport`
/// (`tests/cpp/support/in_process_exchange_transport.hpp`) is the *only*
/// code anywhere that does this; it is exercised by GoogleTest, never
/// linked into `aegis_participant_run` or any production target.
/// `tests/cpp/unit/test_calendar_spread_exchange_integration.cpp` reuses the
/// same transport for AEGIS-004/076..081's M4 strategy.
///
/// The chain under test: OMS intent -> TransportExecutionAdapter -> this
/// test transport -> a real `ExchangeNode` -> real FIFO matching -> the
/// resulting `EmittedEvent`s decoded and fed back into `OrderManager` and
/// `Portfolio`, exactly as a production caller would decode them from the
/// wire. Real M1 matching/FIFO/residual semantics are exercised unmodified.
namespace {

using aegis::common::ManualClock;
using aegis::events::Envelope;
using aegis::events::MessageType;
using aegis::events::exchange::decode_cancel_order;
using aegis::events::exchange::decode_modify_order;
using aegis::events::exchange::decode_new_order;
using aegis::events::exchange::decode_order_accepted;
using aegis::events::exchange::decode_order_modified;
using aegis::events::exchange::decode_order_rejected;
using aegis::events::exchange::decode_order_replaced;
using aegis::events::exchange::decode_order_terminated;
using aegis::events::exchange::decode_trade;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::exchange::EmittedEvent;
using aegis::exchange::ExchangeNode;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;
using aegis::participant::oms::ExecutionTransport;
using aegis::participant::oms::OrderManager;
using aegis::participant::oms::OrderState;
using aegis::participant::oms::RiskDecision;
using aegis::participant::oms::RiskGate;
using aegis::participant::oms::RiskVerdict;
using aegis::participant::oms::TransportExecutionAdapter;
using aegis::participant::portfolio::Portfolio;

constexpr std::uint32_t kInstrumentId = 1;

InstrumentSpec make_spec() {
  InstrumentSpec spec;
  spec.instrument_id = InstrumentId{kInstrumentId};
  spec.price_floor_units = PriceUnits{1000};
  spec.price_ceiling_units = PriceUnits{5000};
  spec.tick_size_units = 25;
  spec.min_quantity_units = QuantityUnits{25};
  spec.max_quantity_units = QuantityUnits{100'000};
  spec.lot_size_units = 25;
  return spec;
}

using aegis::testing::InProcessExchangeTransport;

class AlwaysApproveRiskGate final : public RiskGate {
 public:
  [[nodiscard]] RiskDecision decide(const NewOrderCommand& /*command*/) const override {
    return RiskDecision{
        .verdict = RiskVerdict::kApprove, .resized_quantity_units = 0, .reason = ""};
  }
};

/// Feeds every drained EmittedEvent into `manager` (and, for a trade this
/// participant's own order is one side of, into `portfolio`) exactly as a
/// production feed handler decoding the wire would.
void deliver(const std::vector<EmittedEvent>& emitted, OrderManager& manager, Portfolio& portfolio,
             std::uint64_t our_exchange_order_id) {
  for (const EmittedEvent& event : emitted) {
    switch (event.message_type) {
      case MessageType::kOrderAccepted:
        if (const auto decoded = decode_order_accepted(event.payload); decoded.has_value()) {
          manager.handle_order_accepted(*decoded);
        }
        break;
      case MessageType::kOrderRejected:
        if (const auto decoded = decode_order_rejected(event.payload); decoded.has_value()) {
          manager.handle_order_rejected(*decoded);
        }
        break;
      case MessageType::kOrderModified:
        if (const auto decoded = decode_order_modified(event.payload); decoded.has_value()) {
          // No dedicated handle_order_modified in this slice's OrderManager
          // surface (in-place decreases do not change lifecycle state);
          // tracked-quantity bookkeeping for the modify path is Slice 5's
          // handle_order_replaced (cancel-replace) plus fills, exercised
          // separately.
          static_cast<void>(decoded);
        }
        break;
      case MessageType::kOrderReplaced:
        if (const auto decoded = decode_order_replaced(event.payload); decoded.has_value()) {
          manager.handle_order_replaced(*decoded);
        }
        break;
      case MessageType::kTrade:
        if (const auto decoded = decode_trade(event.payload); decoded.has_value()) {
          manager.handle_trade(*decoded);
          if (decoded->maker_order_id == our_exchange_order_id) {
            portfolio.apply_fill(kInstrumentId, Side::kSell, decoded->price_units,
                                 decoded->quantity_units);
          } else if (decoded->taker_order_id == our_exchange_order_id) {
            portfolio.apply_fill(kInstrumentId, Side::kBuy, decoded->price_units,
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

// AEGIS-109: market-order execution against a real resting counterparty.
TEST(ParticipantExchangeIntegration, MarketOrderConsumesARestingOrderAndReconciles) {
  ExchangeNode node;
  node.add_instrument(make_spec());
  ManualClock clock;

  // A counterparty (participant 2) rests a sell limit order for us to hit.
  const auto counterparty_events =
      node.apply_new_order(NewOrderCommand{.instrument_id = kInstrumentId,
                                           .participant_id = 2,
                                           .client_order_id = 1,
                                           .side = Side::kSell,
                                           .order_type = OrderType::kLimit,
                                           .price_units = 1100,
                                           .quantity_units = 100},
                           node.sequencer().sequence(clock.stamp<aegis::common::EventTime>()));
  ASSERT_EQ(counterparty_events.size(), 1U);

  InProcessExchangeTransport transport(node, clock);
  TransportExecutionAdapter adapter(transport, clock, /*stream_id=*/1);
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  Portfolio portfolio;

  const auto client_order_id =
      manager.submit_new_order(kInstrumentId, /*participant_id=*/1, Side::kBuy, OrderType::kMarket,
                               /*price_units=*/0, /*quantity_units=*/100);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  ASSERT_NE(tracked, nullptr);

  // Feed the acceptance first to learn our exchange order id, then everything else.
  const auto emitted = transport.drain();
  ASSERT_FALSE(emitted.empty());
  const auto accepted = decode_order_accepted(emitted.front().payload);
  ASSERT_TRUE(accepted.has_value());
  deliver(emitted, manager, portfolio, aegis::test::checked(accepted).order_id);

  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kFilled);
  EXPECT_EQ(tracked->cumulative_filled_units, 100);
  EXPECT_EQ(tracked->remaining_units, 0);
  EXPECT_EQ(portfolio.position(kInstrumentId).quantity_units, 100);  // Portfolio reconciles too.
}

// AEGIS-110: passive limit execution -- nonfill, then a later fill.
TEST(ParticipantExchangeIntegration, PassiveLimitOrderRestsThenFillsFromALaterAggressor) {
  ExchangeNode node;
  node.add_instrument(make_spec());
  ManualClock clock;
  InProcessExchangeTransport transport(node, clock);
  TransportExecutionAdapter adapter(transport, clock, /*stream_id=*/1);
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  Portfolio portfolio;

  // A passive limit buy, priced away from any resting liquidity: cannot fill.
  const auto client_order_id =
      manager.submit_new_order(kInstrumentId, /*participant_id=*/1, Side::kBuy, OrderType::kLimit,
                               /*price_units=*/1000, /*quantity_units=*/50);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);

  auto emitted = transport.drain();
  const auto accepted = decode_order_accepted(emitted.front().payload);
  ASSERT_TRUE(accepted.has_value());
  deliver(emitted, manager, portfolio, aegis::test::checked(accepted).order_id);
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kAcknowledged);  // Nonfill case.

  // A later aggressor sells into it.
  const auto aggressor_events =
      node.apply_new_order(NewOrderCommand{.instrument_id = kInstrumentId,
                                           .participant_id = 3,
                                           .client_order_id = 1,
                                           .side = Side::kSell,
                                           .order_type = OrderType::kLimit,
                                           .price_units = 1000,
                                           .quantity_units = 50},
                           node.sequencer().sequence(clock.stamp<aegis::common::EventTime>()));
  deliver(aggressor_events, manager, portfolio, aegis::test::checked(accepted).order_id);

  // The maker's OrderTerminatedEvent{kFilled} arrives in the same batch as
  // the trade (match_and_apply_fills emits both together), so a fully
  // consumed resting order reaches kFilled directly -- consistent with the
  // market-order case above.
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kFilled);
  EXPECT_EQ(tracked->cumulative_filled_units, 50);
  EXPECT_EQ(tracked->remaining_units, 0);
}

// AEGIS-111: aggressive limit execution across multiple price levels.
TEST(ParticipantExchangeIntegration, AggressiveLimitOrderSweepsMultipleLevelsWithAResidual) {
  ExchangeNode node;
  node.add_instrument(make_spec());
  ManualClock clock;

  // Two resting sell levels, best first.
  static_cast<void>(
      node.apply_new_order(NewOrderCommand{.instrument_id = kInstrumentId,
                                           .participant_id = 2,
                                           .client_order_id = 1,
                                           .side = Side::kSell,
                                           .order_type = OrderType::kLimit,
                                           .price_units = 1100,
                                           .quantity_units = 50},
                           node.sequencer().sequence(clock.stamp<aegis::common::EventTime>())));
  static_cast<void>(
      node.apply_new_order(NewOrderCommand{.instrument_id = kInstrumentId,
                                           .participant_id = 2,
                                           .client_order_id = 2,
                                           .side = Side::kSell,
                                           .order_type = OrderType::kLimit,
                                           .price_units = 1125,
                                           .quantity_units = 50},
                           node.sequencer().sequence(clock.stamp<aegis::common::EventTime>())));

  InProcessExchangeTransport transport(node, clock);
  TransportExecutionAdapter adapter(transport, clock, /*stream_id=*/1);
  AlwaysApproveRiskGate risk;
  OrderManager manager(adapter, risk);
  Portfolio portfolio;

  // Aggressive limit buy: crosses both levels (up to 1125), leaves a residual
  // resting. 125 = 100 (both resting levels) + 25 (one more lot residual) --
  // the instrument's lot_size_units is 25, so every quantity here must be a
  // multiple of it.
  const auto client_order_id =
      manager.submit_new_order(kInstrumentId, /*participant_id=*/1, Side::kBuy, OrderType::kLimit,
                               /*price_units=*/1125, /*quantity_units=*/125);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);

  const auto emitted = transport.drain();
  const auto accepted = decode_order_accepted(emitted.front().payload);
  ASSERT_TRUE(accepted.has_value());
  deliver(emitted, manager, portfolio, aegis::test::checked(accepted).order_id);

  EXPECT_EQ(tracked->cumulative_filled_units, 100);  // 50 + 50 across both levels.
  EXPECT_EQ(tracked->remaining_units, 25);           // Residual rests (limit, not market).
  EXPECT_EQ(tracked->lifecycle.state(), OrderState::kPartiallyFilled);
  EXPECT_EQ(portfolio.position(kInstrumentId).quantity_units, 100);
}

}  // namespace
