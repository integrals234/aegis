#include "cpp/exchange/matching/fifo_policy.hpp"

#include "cpp/exchange/order_book/level_index.hpp"

namespace aegis::exchange {

namespace {

[[nodiscard]] bool crosses(Side aggressor_side, PriceUnits maker_price, PriceUnits limit_price) {
  // A buy aggressor crosses a maker priced at or below its limit; a sell
  // aggressor crosses a maker priced at or above its limit.
  return aggressor_side == Side::kBuy ? maker_price <= limit_price : maker_price >= limit_price;
}

}  // namespace

std::vector<Pairing> FifoPolicy::match(const OrderBook& book, Side aggressor_side,
                                       std::optional<PriceUnits> limit_price,
                                       QuantityUnits quantity) const {
  std::vector<Pairing> pairings;
  const Side maker_side = aggressor_side == Side::kBuy ? Side::kSell : Side::kBuy;
  const LevelIndex& maker_levels = book.levels(maker_side);

  QuantityUnits remaining = quantity;
  std::optional<PriceUnits> price = maker_levels.best_price();

  while (remaining.value() > 0 && price.has_value()) {
    if (limit_price.has_value() && !crosses(aggressor_side, *price, *limit_price)) {
      break;
    }

    const PriceLevel* level = book.level_at(maker_side, *price);
    if (level != nullptr) {
      std::size_t index = level->head_index;
      while (index != kInvalidIndex && remaining.value() > 0) {
        const OrderNode& maker = book.storage().node(index);
        const QuantityUnits fill = min(maker.remaining, remaining);
        pairings.push_back(Pairing{.maker_order_id = maker.order_id, .fill_quantity = fill});
        remaining -= fill;
        index = maker.next_index;
      }
    }

    if (remaining.value() == 0) {
      break;
    }
    price = maker_levels.next_price_after(*price);
  }

  return pairings;
}

}  // namespace aegis::exchange
