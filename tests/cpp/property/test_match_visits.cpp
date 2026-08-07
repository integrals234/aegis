#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/exchange/matching/fifo_policy.hpp"
#include "cpp/exchange/order_book/book.hpp"

/// AEGIS-038, AEGIS-039: matching is output-sensitive — the number of
/// resting orders `FifoPolicy::match` visits equals the number it actually
/// consumes (every pairing but possibly the last is a full fill of that
/// maker; the aggressor's quantity is exhausted, or the book/price bound
/// is), never more. A seeded PRNG builds books of varying depth and checks
/// this over many generated scenarios.
namespace {

using aegis::exchange::ClientOrderId;
using aegis::exchange::CommandSequence;
using aegis::exchange::FifoPolicy;
using aegis::exchange::InstrumentId;
using aegis::exchange::OrderBook;
using aegis::exchange::OrderId;
using aegis::exchange::OrderNode;
using aegis::exchange::OrderType;
using aegis::exchange::ParticipantId;
using aegis::exchange::PriceUnits;
using aegis::exchange::Priority;
using aegis::exchange::QuantityUnits;
using aegis::exchange::Side;

OrderNode make_resting(std::uint64_t id, Side side, PriceUnits price, QuantityUnits quantity) {
  OrderNode node;
  node.order_id = OrderId{id};
  node.instrument_id = InstrumentId{1};
  node.participant_id = ParticipantId{id};
  node.client_order_id = ClientOrderId{id};
  node.side = side;
  node.order_type = OrderType::kLimit;
  node.price_units = price;
  node.original_quantity = quantity;
  node.remaining = quantity;
  node.priority = Priority::from(CommandSequence{id});
  return node;
}

TEST(MatchVisits, PairingsNeverExceedWhatIsNeededToSatisfyTheAggressor) {
  // A fixed seed is deliberate (AEGIS-005): this test's whole point is a
  // reproducible generated sequence, not cryptographic unpredictability —
  // the failure this check guards against does not apply here.
  std::mt19937 rng{7};  // NOLINT(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::uniform_int_distribution<int> level_count_dist(1, 6);
  std::uniform_int_distribution<int> orders_per_level_dist(1, 4);
  std::uniform_int_distribution<std::int64_t> lot_multiple_dist(1, 4);
  std::uniform_int_distribution<std::int64_t> aggressor_lots_dist(1, 30);

  constexpr int kScenarios = 300;
  const FifoPolicy policy;

  for (int scenario = 0; scenario < kScenarios; ++scenario) {
    OrderBook book{InstrumentId{1}};
    std::uint64_t next_id = 1;
    const int levels = level_count_dist(rng);
    QuantityUnits total_resting{0};

    for (int level = 0; level < levels; ++level) {
      const int orders_here = orders_per_level_dist(rng);
      const PriceUnits price{1000 + (level * 25)};
      for (int i = 0; i < orders_here; ++i) {
        const QuantityUnits quantity{lot_multiple_dist(rng) * 50};
        book.add(make_resting(next_id++, Side::kSell, price, quantity));
        total_resting += quantity;
      }
    }

    const QuantityUnits aggressor_quantity{aggressor_lots_dist(rng) * 50};
    const auto pairings = policy.match(book, Side::kBuy, std::nullopt, aggressor_quantity);

    // No pairing is ever for zero quantity, and no maker id repeats — each
    // resting order is visited (and consumed) at most once per match() call.
    std::vector<std::uint64_t> seen;
    QuantityUnits filled{0};
    for (const auto& pairing : pairings) {
      EXPECT_GT(pairing.fill_quantity.value(), 0) << "scenario " << scenario;
      for (const auto seen_id : seen) {
        EXPECT_NE(seen_id, pairing.maker_order_id.value()) << "scenario " << scenario;
      }
      seen.push_back(pairing.maker_order_id.value());
      filled += pairing.fill_quantity;
    }

    // The aggressor is satisfied exactly when possible: filled quantity is
    // the lesser of what it asked for and what the book could offer.
    const QuantityUnits expected_filled = aegis::exchange::min(aggressor_quantity, total_resting);
    EXPECT_EQ(filled, expected_filled) << "scenario " << scenario;

    // Output-sensitivity: every pairing but the last fully consumes its
    // maker (remaining == 0 in the book's own bookkeeping is not observable
    // here since match() never mutates the book — but a partial fill can
    // only legitimately be the very last pairing, since the aggressor's
    // remaining quantity would otherwise still be nonzero and matching
    // would continue to the next order).
    for (std::size_t i = 0; i + 1 < pairings.size(); ++i) {
      const auto* maker = book.find(pairings[i].maker_order_id);
      ASSERT_NE(maker, nullptr) << "scenario " << scenario;
      EXPECT_EQ(pairings[i].fill_quantity, maker->remaining)
          << "scenario " << scenario << ": only the last pairing may be a partial fill";
    }
  }
}

}  // namespace
