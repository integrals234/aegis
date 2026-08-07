#include <cstdint>

#include <gtest/gtest.h>

#include "cpp/exchange/matching/engine.hpp"
#include "cpp/exchange/matching/fifo_policy.hpp"

/// AEGIS-035, ADR-0011: duplicate detection scope is `(participant_id,
/// client_order_id)`, not `client_order_id` alone, and is enforced over live
/// orders only — reuse after termination is accepted, and there is no
/// tombstone set remembering which ids have ever been used.
namespace {

using aegis::events::CommandSequence;
using aegis::events::MessageType;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::decode_order_rejected;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderRejectedEvent;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::RejectReason;
using aegis::events::exchange::Side;
using aegis::exchange::EmittedEvent;
using aegis::exchange::FifoPolicy;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::MatchingEngine;
using aegis::exchange::OrderBook;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;

InstrumentSpec make_spec() {
  return InstrumentSpec{
      .instrument_id = InstrumentId{1},
      .price_floor_units = PriceUnits{1000},
      .price_ceiling_units = PriceUnits{2000},
      .tick_size_units = 25,
      .min_quantity_units = QuantityUnits{50},
      .max_quantity_units = QuantityUnits{100000},
      .lot_size_units = 50,
  };
}

NewOrderCommand make_limit(Side side, std::uint64_t participant_id, std::uint64_t client_order_id) {
  return NewOrderCommand{
      .instrument_id = 1,
      .participant_id = participant_id,
      .client_order_id = client_order_id,
      .side = side,
      .order_type = OrderType::kLimit,
      .price_units = 1000,
      .quantity_units = 100,
  };
}

/// Not a GTest fixture: see the identical note in test_limit_orders.cpp.
struct Harness {
  InstrumentSpec spec = make_spec();
  OrderBook book{InstrumentId{1}};
  FifoPolicy policy;
  MatchingEngine engine{policy};
  std::uint64_t next_sequence{1};

  std::vector<EmittedEvent> new_order(const NewOrderCommand& command) {
    return engine.apply_new_order(book, spec, command, CommandSequence{next_sequence++});
  }
  std::vector<EmittedEvent> cancel(std::uint64_t participant_id, std::uint64_t order_id) {
    return MatchingEngine::apply_cancel_order(
        book,
        CancelOrderCommand{
            .instrument_id = 1, .participant_id = participant_id, .order_id = order_id},
        CommandSequence{next_sequence++});
  }
};

TEST(ClientOrderIdScope, TwoParticipantsMayShareTheSameClientOrderId) {
  Harness h;
  // Both resting on the same (buy) side so they do not cross each other —
  // this test is about identity scope, not matching.
  const auto first = h.new_order(make_limit(Side::kBuy, 10, 1));
  const auto second =
      h.new_order(make_limit(Side::kBuy, 20, 1));  // same client id, different owner

  EXPECT_EQ(first[0].message_type, MessageType::kOrderAccepted);
  EXPECT_EQ(second[0].message_type, MessageType::kOrderAccepted);
  EXPECT_EQ(h.book.live_order_count(), 2U);
}

TEST(ClientOrderIdScope, SameParticipantReusingAClientOrderIdWhileLiveIsRejected) {
  Harness h;
  h.new_order(make_limit(Side::kBuy, 10, 1));                      // OrderId 1, live
  const auto events = h.new_order(make_limit(Side::kBuy, 10, 1));  // same (participant, client)

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].message_type, MessageType::kOrderRejected);
  const auto rejected = decode_order_rejected(events[0].payload);
  EXPECT_EQ(rejected.value_or(OrderRejectedEvent{}).reason, RejectReason::kDuplicateClientOrderId);
  EXPECT_EQ(h.book.live_order_count(), 1U) << "the rejected duplicate must not be created";
}

TEST(ClientOrderIdScope, ReuseAfterCancellationIsAccepted) {
  Harness h;
  const auto first = h.new_order(make_limit(Side::kBuy, 10, 1));  // OrderId 1
  h.cancel(10, 1);                                                // OrderId 1 terminates

  const auto second =
      h.new_order(make_limit(Side::kBuy, 10, 1));  // same (participant, client) again
  EXPECT_EQ(first[0].message_type, MessageType::kOrderAccepted);
  ASSERT_EQ(second.size(), 1U);
  EXPECT_EQ(second[0].message_type, MessageType::kOrderAccepted)
      << "a client order id is reusable once its order has terminated (ADR-0011): "
         "no unbounded tombstone set remembers it was ever used";
  EXPECT_EQ(h.book.live_order_count(), 1U);
}

TEST(ClientOrderIdScope, ReuseAfterFullFillIsAccepted) {
  Harness h;
  h.new_order(make_limit(Side::kSell, 10, 1));  // OrderId 1
  h.new_order(make_limit(Side::kBuy, 20, 1));   // fills OrderId 1 fully; participant 20's own id

  // participant 10's client id 1 is free again — its order filled, not cancelled.
  const auto events = h.new_order(make_limit(Side::kSell, 10, 1));
  EXPECT_EQ(events[0].message_type, MessageType::kOrderAccepted);
}

}  // namespace
