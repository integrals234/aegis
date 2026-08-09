#include "cpp/exchange/order_book/level_index.hpp"

namespace aegis::exchange {

PriceLevel& MapLevelIndex::level_at(PriceUnits price) {
  auto [it, inserted] = levels_.try_emplace(price);
  if (inserted) {
    it->second.price = price;
  }
  return it->second;
}

PriceLevel* MapLevelIndex::find(PriceUnits price) {
  const auto it = levels_.find(price);
  return it == levels_.end() ? nullptr : &it->second;
}

const PriceLevel* MapLevelIndex::find(PriceUnits price) const {
  const auto it = levels_.find(price);
  return it == levels_.end() ? nullptr : &it->second;
}

void MapLevelIndex::erase(PriceUnits price) { levels_.erase(price); }

std::optional<PriceUnits> MapLevelIndex::best_price() const {
  if (levels_.empty()) {
    return std::nullopt;
  }
  return levels_.begin()->first;
}

std::optional<PriceUnits> MapLevelIndex::next_price_after(PriceUnits price) const {
  const auto it = levels_.upper_bound(price);
  if (it == levels_.end()) {
    return std::nullopt;
  }
  return it->first;
}

bool MapLevelIndex::empty() const { return levels_.empty(); }

}  // namespace aegis::exchange
