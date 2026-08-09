#include <cstdint>
#include <optional>
#include <random>
#include <unordered_map>

#include <gtest/gtest.h>

#include "cpp/exchange/app/exchange_node.hpp"

/// P1-P4 (plan §4.10, AEGIS-033) over a generated command sequence. The
/// ledger is accumulated purely from the decoded `EmittedEvent` stream, never
/// from `OrderBook`/`OrderNode` internals, so a bug that corrupts both the
/// book and its own accounting in the same way is still caught: P1 and P4 are
/// self-consistency checks on the ledger alone, and P3 cross-checks the
/// ledger's independently-tracked `remaining` against the book's own
/// per-level aggregates, which is the one place the two are compared.
namespace {

using aegis::common::EventTime;
using aegis::events::MessageType;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::decode_order_accepted;
using aegis::events::exchange::decode_order_modified;
using aegis::events::exchange::decode_order_replaced;
using aegis::events::exchange::decode_order_terminated;
using aegis::events::exchange::decode_trade;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::exchange::EmittedEvent;
using aegis::exchange::ExchangeNode;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;

constexpr std::int64_t kLotSizeUnits = 50;

InstrumentSpec make_spec() {
  return InstrumentSpec{
      .instrument_id = InstrumentId{1},
      .price_floor_units = PriceUnits{1000},
      .price_ceiling_units = PriceUnits{2000},
      .tick_size_units = 25,
      .min_quantity_units = QuantityUnits{50},
      .max_quantity_units = QuantityUnits{5000},
      .lot_size_units = kLotSizeUnits,
  };
}

struct LedgerEntry {
  Side side{Side::kBuy};
  std::int64_t original_quantity{0};
  std::int64_t cumulative_filled{0};
  std::int64_t remaining{0};
  std::int64_t cancelled_quantity{0};
  bool terminated{false};
};

bool is_lot_aligned(std::int64_t quantity) { return quantity % kLotSizeUnits == 0; }

class Ledger {
 public:
  void apply(const EmittedEvent& event) {
    switch (event.message_type) {
      case MessageType::kOrderAccepted: {
        const auto decoded_opt = decode_order_accepted(event.payload);
        ASSERT_TRUE(decoded_opt.has_value());
        const auto decoded = decoded_opt.value_or(aegis::events::exchange::OrderAcceptedEvent{});
        auto& entry = entries_[decoded.order_id];
        entry.side = decoded.side;
        entry.original_quantity = decoded.quantity_units;
        entry.remaining = decoded.quantity_units;
        break;
      }
      case MessageType::kTrade: {
        const auto decoded_opt = decode_trade(event.payload);
        ASSERT_TRUE(decoded_opt.has_value());
        const auto decoded = decoded_opt.value_or(aegis::events::exchange::TradeEvent{});
        total_traded_ += decoded.quantity_units;
        for (const auto order_id : {decoded.maker_order_id, decoded.taker_order_id}) {
          auto& entry = entries_.at(order_id);
          entry.cumulative_filled += decoded.quantity_units;
          entry.remaining -= decoded.quantity_units;
        }
        break;
      }
      case MessageType::kOrderModified: {
        const auto decoded_opt = decode_order_modified(event.payload);
        ASSERT_TRUE(decoded_opt.has_value());
        const auto decoded = decoded_opt.value_or(aegis::events::exchange::OrderModifiedEvent{});
        auto& entry = entries_.at(decoded.order_id);
        entry.remaining = decoded.new_remaining_units;
        entry.cancelled_quantity += decoded.cancelled_quantity_delta_units;
        break;
      }
      case MessageType::kOrderReplaced: {
        const auto decoded_opt = decode_order_replaced(event.payload);
        ASSERT_TRUE(decoded_opt.has_value());
        const auto decoded = decoded_opt.value_or(aegis::events::exchange::OrderReplacedEvent{});
        auto& entry = entries_[decoded.new_order_id];
        entry.side = decoded.side;
        entry.original_quantity = decoded.quantity_units;
        entry.remaining = decoded.quantity_units;
        break;
      }
      case MessageType::kOrderTerminated: {
        const auto decoded_opt = decode_order_terminated(event.payload);
        ASSERT_TRUE(decoded_opt.has_value());
        const auto decoded = decoded_opt.value_or(aegis::events::exchange::OrderTerminatedEvent{});
        auto& entry = entries_.at(decoded.order_id);
        entry.cancelled_quantity += decoded.cancelled_quantity_delta_units;
        entry.remaining = 0;
        entry.terminated = true;
        break;
      }
      default:
        break;  // kOrderRejected and command types: no order created.
    }
  }

  // P1 — per-order conservation, checked at every quiescent point.
  void check_p1() const {
    for (const auto& [order_id, entry] : entries_) {
      EXPECT_EQ(entry.original_quantity,
                entry.cumulative_filled + entry.remaining + entry.cancelled_quantity)
          << "P1 violated for order " << order_id;
    }
  }

  // P4 — lot alignment, checked at every quiescent point.
  void check_p4() const {
    for (const auto& [order_id, entry] : entries_) {
      EXPECT_TRUE(is_lot_aligned(entry.remaining)) << "order " << order_id << " remaining off-lot";
      EXPECT_TRUE(is_lot_aligned(entry.cumulative_filled))
          << "order " << order_id << " cumulative_filled off-lot";
      EXPECT_TRUE(is_lot_aligned(entry.cancelled_quantity))
          << "order " << order_id << " cancelled_quantity off-lot";
    }
  }

  // P3 — book aggregate: the ledger's independently-tracked sum of `remaining`
  // over live orders on `side` must equal the book's own per-level aggregate
  // total, walked via the public `LevelIndex` interface.
  void check_p3(const aegis::exchange::OrderBook& book, Side side) const {
    std::int64_t ledger_sum = 0;
    for (const auto& [order_id, entry] : entries_) {
      if (!entry.terminated && entry.side == side) {
        ledger_sum += entry.remaining;
      }
    }

    std::int64_t book_sum = 0;
    const auto& level_index = book.levels(side);
    std::optional<PriceUnits> price = level_index.best_price();
    while (price.has_value()) {
      const auto* level = book.level_at(side, *price);
      ASSERT_NE(level, nullptr);
      book_sum += level->aggregate_quantity.value();
      EXPECT_TRUE(is_lot_aligned(level->aggregate_quantity.value())) << "level aggregate off-lot";
      price = level_index.next_price_after(*price);
    }

    EXPECT_EQ(ledger_sum, book_sum) << "P3 violated on side " << static_cast<int>(side);
  }

  // P2 — aggregate conservation counting both sides, checked once at the end.
  void check_p2() const {
    std::int64_t buy_filled = 0;
    std::int64_t sell_filled = 0;
    for (const auto& [order_id, entry] : entries_) {
      if (entry.side == Side::kBuy) {
        buy_filled += entry.cumulative_filled;
      } else {
        sell_filled += entry.cumulative_filled;
      }
    }
    EXPECT_EQ(buy_filled, total_traded_);
    EXPECT_EQ(sell_filled, total_traded_);
    EXPECT_EQ(buy_filled + sell_filled, 2 * total_traded_);
  }

 private:
  std::unordered_map<std::uint64_t, LedgerEntry> entries_;
  std::int64_t total_traded_{0};
};

TEST(QuantityConservation, P1ThroughP4HoldOverGeneratedSequence) {
  // Fixed seed is deliberate (AEGIS-005): reproducibility is the point.
  std::mt19937 rng{1729};  // NOLINT(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::uniform_int_distribution<int> command_choice(0, 2);  // 0=new, 1=cancel, 2=modify
  std::uniform_int_distribution<int> side_choice(0, 1);
  std::uniform_int_distribution<int> order_type_choice(0, 9);  // mostly limit, some market
  std::uniform_int_distribution<std::int64_t> price_lot_dist(0, 40);
  std::uniform_int_distribution<std::int64_t> quantity_lot_dist(1, 20);
  std::uniform_int_distribution<std::uint64_t> participant_dist(1, 6);
  std::uniform_int_distribution<std::uint64_t> target_order_dist(1, 250);

  ExchangeNode node;
  node.add_instrument(make_spec());
  Ledger ledger;

  constexpr int kCommands = 800;
  std::uint64_t next_client_order_id = 1;

  for (int i = 0; i < kCommands; ++i) {
    const auto command_sequence = node.sequencer().sequence(EventTime{i});
    const auto participant = participant_dist(rng);
    std::vector<EmittedEvent> events;

    switch (command_choice(rng)) {
      case 0: {
        const bool is_market = order_type_choice(rng) == 0;
        events = node.apply_new_order(
            NewOrderCommand{
                .instrument_id = 1,
                .participant_id = participant,
                .client_order_id = next_client_order_id++,
                .side = side_choice(rng) == 0 ? Side::kBuy : Side::kSell,
                .order_type = is_market ? OrderType::kMarket : OrderType::kLimit,
                .price_units = is_market ? 0 : 1000 + (price_lot_dist(rng) * 25),
                .quantity_units = quantity_lot_dist(rng) * kLotSizeUnits,
            },
            command_sequence);
        break;
      }
      case 1: {
        events = node.apply_cancel_order(CancelOrderCommand{.instrument_id = 1,
                                                            .participant_id = participant,
                                                            .order_id = target_order_dist(rng)},
                                         command_sequence);
        break;
      }
      default: {
        events = node.apply_modify_order(
            ModifyOrderCommand{.instrument_id = 1,
                               .participant_id = participant,
                               .order_id = target_order_dist(rng),
                               .new_price_units = 1000 + (price_lot_dist(rng) * 25),
                               .new_quantity_units = quantity_lot_dist(rng) * kLotSizeUnits},
            command_sequence);
        break;
      }
    }

    for (const auto& event : events) {
      ledger.apply(event);
    }

    ledger.check_p1();
    ledger.check_p4();
    const auto* book = node.book(InstrumentId{1});
    ASSERT_NE(book, nullptr);
    ledger.check_p3(*book, Side::kBuy);
    ledger.check_p3(*book, Side::kSell);
  }

  ledger.check_p2();
}

}  // namespace
