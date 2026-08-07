#include <cstdint>
#include <optional>
#include <tuple>

#include <gtest/gtest.h>

#include "cpp/exchange/matching/engine.hpp"
#include "cpp/exchange/matching/fifo_policy.hpp"

/// AEGIS-040: the architecture permits later FIFO/pro-rata variants without
/// contaminating the FIFO core. This test proves the seam is real — a
/// second, deliberately non-FIFO policy plugs into the exact same
/// `MatchingEngine` — without shipping pro-rata itself (that stays out of
/// scope for M1, per experiments/plans/M1.md §3).
namespace {

using aegis::events::CommandSequence;
using aegis::events::MessageType;
using aegis::events::exchange::decode_trade;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::exchange::FifoPolicy;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::MatchingEngine;
using aegis::exchange::MatchingPolicy;
using aegis::exchange::OrderBook;
using aegis::exchange::Pairing;
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

/// A deliberately non-FIFO policy: it matches only the single best-priced
/// resting order, capping the fill at that order's own remaining quantity —
/// no multi-level sweep, no FIFO tie-break within a level. It exists only to
/// prove `MatchingEngine` is not hardwired to `FifoPolicy`; it is not a
/// pro-rata implementation and ships no requirement of its own.
class SingleBestOrderOnlyPolicy final : public MatchingPolicy {
 public:
  [[nodiscard]] std::vector<Pairing> match(const OrderBook& book, Side aggressor_side,
                                           std::optional<PriceUnits> limit_price,
                                           QuantityUnits quantity) const override {
    const Side maker_side = aggressor_side == Side::kBuy ? Side::kSell : Side::kBuy;
    const auto best = book.levels(maker_side).best_price();
    if (!best.has_value()) {
      return {};
    }
    if (limit_price.has_value()) {
      const bool crosses =
          aggressor_side == Side::kBuy ? *best <= *limit_price : *best >= *limit_price;
      if (!crosses) {
        return {};
      }
    }
    const auto makers = book.orders_at(maker_side, *best);
    if (makers.empty()) {
      return {};
    }
    const auto* maker = book.find(makers.front());
    if (maker == nullptr) {
      return {};
    }
    return {Pairing{.maker_order_id = makers.front(),
                    .fill_quantity = aegis::exchange::min(maker->remaining, quantity)}};
  }
};

TEST(MatchingPolicyInterface, EngineWorksWithAnyPolicyImplementation) {
  InstrumentSpec spec = make_spec();
  OrderBook book{InstrumentId{1}};
  const SingleBestOrderOnlyPolicy policy;
  MatchingEngine engine{policy};
  std::uint64_t sequence = 1;

  std::ignore = engine.apply_new_order(book, spec, make_limit(Side::kSell, 1000, 50, 10, 1),
                                       CommandSequence{sequence++});
  const auto events = engine.apply_new_order(book, spec, make_limit(Side::kBuy, 1000, 50, 20, 1),
                                             CommandSequence{sequence++});

  bool traded = false;
  for (const auto& event : events) {
    if (event.message_type == MessageType::kTrade) {
      traded = true;
      const auto trade = decode_trade(event.payload);
      EXPECT_EQ(
          trade.value_or(aegis::events::exchange::TradeEvent{.maker_order_id = 0}).maker_order_id,
          1U);
    }
  }
  EXPECT_TRUE(traded) << "MatchingEngine must drive whatever MatchingPolicy it was given";
}

TEST(MatchingPolicyInterface, NonFifoPolicyProducesVisiblyDifferentPairingsFromFifoPolicy) {
  // Two resting sells at the same price; a FIFO policy would fill both in
  // arrival order for a large-enough aggressor, but the single-best-only
  // policy caps the fill at the first order's own quantity and stops —
  // proving the two policies are genuinely different decisions, not the same
  // logic under a different name.
  InstrumentSpec spec = make_spec();
  OrderBook fifo_book{InstrumentId{1}};
  OrderBook single_book{InstrumentId{1}};
  const FifoPolicy fifo_policy;
  const SingleBestOrderOnlyPolicy single_policy;
  MatchingEngine fifo_engine{fifo_policy};
  MatchingEngine single_engine{single_policy};

  std::uint64_t fifo_sequence = 1;
  std::uint64_t single_sequence = 1;
  for (auto* book : {&fifo_book, &single_book}) {
    auto& engine = (book == &fifo_book) ? fifo_engine : single_engine;
    auto& sequence = (book == &fifo_book) ? fifo_sequence : single_sequence;
    std::ignore = engine.apply_new_order(*book, spec, make_limit(Side::kSell, 1000, 50, 10, 1),
                                         CommandSequence{sequence++});
    std::ignore = engine.apply_new_order(*book, spec, make_limit(Side::kSell, 1000, 50, 11, 2),
                                         CommandSequence{sequence++});
  }

  const auto fifo_events = fifo_engine.apply_new_order(
      fifo_book, spec, make_limit(Side::kBuy, 1000, 100, 20, 1), CommandSequence{fifo_sequence++});
  const auto single_events =
      single_engine.apply_new_order(single_book, spec, make_limit(Side::kBuy, 1000, 100, 20, 1),
                                    CommandSequence{single_sequence++});

  int fifo_trade_count = 0;
  for (const auto& event : fifo_events) {
    fifo_trade_count += (event.message_type == MessageType::kTrade) ? 1 : 0;
  }
  int single_trade_count = 0;
  for (const auto& event : single_events) {
    single_trade_count += (event.message_type == MessageType::kTrade) ? 1 : 0;
  }

  EXPECT_EQ(fifo_trade_count, 2) << "FIFO sweeps both resting orders to satisfy the aggressor";
  EXPECT_EQ(single_trade_count, 1) << "the single-best-only policy stops after the first order";
}

}  // namespace
