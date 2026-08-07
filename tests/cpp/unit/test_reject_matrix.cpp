#include <array>
#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "cpp/exchange/matching/engine.hpp"
#include "cpp/exchange/matching/fifo_policy.hpp"

/// AEGIS-035: the reject taxonomy is a closed set (ADR-0011), and every
/// enumerator must be reachable. `trigger()` below switches over every
/// `RejectReason` with no `default:` label, so adding a new enumerator
/// without adding a matching case here fails to compile (-Wswitch,
/// `AEGIS_WERROR`) — the same mechanism `cpp/events/exchange_messages.cpp`'s
/// `describe(RejectReason)` already uses.
namespace {

using aegis::events::CommandSequence;
using aegis::events::exchange::decode_new_order;
using aegis::events::exchange::decode_order_rejected;
using aegis::events::exchange::encode;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::RejectReason;
using aegis::events::exchange::Side;
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

NewOrderCommand make_limit(Side side, std::int64_t price, std::int64_t quantity,
                           std::uint64_t participant_id, std::uint64_t client_order_id) {
  return NewOrderCommand{
      .instrument_id = 1,
      .participant_id = participant_id,
      .client_order_id = client_order_id,
      .side = side,
      .order_type = OrderType::kLimit,
      .price_units = price,
      .quantity_units = quantity,
  };
}

/// Not a GTest fixture: see the identical note in test_limit_orders.cpp.
struct Harness {
  InstrumentSpec spec = make_spec();
  OrderBook book{InstrumentId{1}};
  FifoPolicy policy;
  MatchingEngine engine{policy};
  std::uint64_t next_sequence{1};

  std::vector<aegis::exchange::EmittedEvent> new_order(const NewOrderCommand& command) {
    return engine.apply_new_order(book, spec, command, CommandSequence{next_sequence++});
  }
  std::vector<aegis::exchange::EmittedEvent> modify(const ModifyOrderCommand& command) {
    return engine.apply_modify_order(book, spec, command, CommandSequence{next_sequence++});
  }
};

std::optional<RejectReason> reason_of(const std::vector<aegis::exchange::EmittedEvent>& events) {
  if (events.empty() || events.back().message_type != aegis::events::MessageType::kOrderRejected) {
    return std::nullopt;
  }
  const auto decoded = decode_order_rejected(events.back().payload);
  if (!decoded.has_value()) {
    return std::nullopt;
  }
  return decoded.value().reason;
}

std::optional<RejectReason> trigger(RejectReason target) {
  switch (target) {
    case RejectReason::kUnknownInstrument: {
      // Exercised at the ExchangeNode routing layer (test_exchange_node.cpp),
      // not through MatchingEngine directly: there is no book to hand it an
      // unknown instrument, so this row documents the reason exists rather
      // than re-deriving ExchangeNode's own dedicated test.
      return RejectReason::kUnknownInstrument;
    }
    case RejectReason::kDuplicateClientOrderId: {
      Harness h;
      h.new_order(make_limit(Side::kBuy, 1000, 100, 10, 1));
      return reason_of(h.new_order(make_limit(Side::kBuy, 1000, 100, 10, 1)));
    }
    case RejectReason::kNonPositiveQuantity: {
      Harness h;
      return reason_of(h.new_order(make_limit(Side::kBuy, 1000, 0, 1, 1)));
    }
    case RejectReason::kQuantityNotOnLot: {
      Harness h;
      return reason_of(h.new_order(make_limit(Side::kBuy, 1000, 130, 1, 1)));
    }
    case RejectReason::kQuantityOutOfRange: {
      Harness h;
      return reason_of(h.new_order(make_limit(Side::kBuy, 1000, 200000, 1, 1)));
    }
    case RejectReason::kPriceNotOnTick: {
      Harness h;
      return reason_of(h.new_order(make_limit(Side::kBuy, 1010, 100, 1, 1)));
    }
    case RejectReason::kPriceOutOfBand: {
      Harness h;
      return reason_of(h.new_order(make_limit(Side::kBuy, 3000, 100, 1, 1)));
    }
    case RejectReason::kPriceOnMarketOrder: {
      Harness h;
      NewOrderCommand market{.instrument_id = 1,
                             .participant_id = 1,
                             .client_order_id = 1,
                             .side = Side::kBuy,
                             .order_type = OrderType::kMarket,
                             .price_units = 1000,
                             .quantity_units = 100};
      return reason_of(h.new_order(market));
    }
    case RejectReason::kUnknownOrderId: {
      Harness h;
      return reason_of(h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                   .participant_id = 1,
                                                   .order_id = 999,
                                                   .new_price_units = 1000,
                                                   .new_quantity_units = 100}));
    }
    case RejectReason::kNotOrderOwner: {
      Harness h;
      h.new_order(make_limit(Side::kBuy, 1000, 100, 10, 1));  // OrderId 1
      return reason_of(h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                   .participant_id = 999,
                                                   .order_id = 1,
                                                   .new_price_units = 1000,
                                                   .new_quantity_units = 100}));
    }
    case RejectReason::kModifyBelowFilled: {
      Harness h;
      h.new_order(make_limit(Side::kSell, 1000, 150, 10, 1));  // OrderId 1
      h.new_order(make_limit(Side::kBuy, 1000, 100, 20, 1));   // fills 100
      return reason_of(h.modify(ModifyOrderCommand{.instrument_id = 1,
                                                   .participant_id = 10,
                                                   .order_id = 1,
                                                   .new_price_units = 1000,
                                                   .new_quantity_units = 50}));
    }
    case RejectReason::kMalformedMessage: {
      // The wire precondition slice 10's replay producer translates into
      // this reason: a command that fails to decode at all. There is no
      // MatchingEngine path to it — a command that never decoded never
      // reaches the engine.
      const NewOrderCommand well_formed{.instrument_id = 1,
                                        .participant_id = 1,
                                        .client_order_id = 1,
                                        .side = Side::kBuy,
                                        .order_type = OrderType::kLimit,
                                        .price_units = 1000,
                                        .quantity_units = 100};
      auto bytes = encode(well_formed);
      bytes.pop_back();  // truncate: must fail to decode
      return decode_new_order(bytes).has_value() ? std::nullopt
                                                 : std::optional{RejectReason::kMalformedMessage};
    }
  }
  return std::nullopt;  // unreachable if the switch above is exhaustive
}

constexpr std::array<RejectReason, 12> kAllReasons{
    RejectReason::kUnknownInstrument,   RejectReason::kDuplicateClientOrderId,
    RejectReason::kNonPositiveQuantity, RejectReason::kQuantityNotOnLot,
    RejectReason::kQuantityOutOfRange,  RejectReason::kPriceNotOnTick,
    RejectReason::kPriceOutOfBand,      RejectReason::kPriceOnMarketOrder,
    RejectReason::kUnknownOrderId,      RejectReason::kNotOrderOwner,
    RejectReason::kModifyBelowFilled,   RejectReason::kMalformedMessage,
};

TEST(RejectMatrix, EveryReasonIsReachable) {
  for (const auto expected : kAllReasons) {
    EXPECT_EQ(trigger(expected), expected) << "RejectReason " << static_cast<int>(expected);
  }
}

}  // namespace
