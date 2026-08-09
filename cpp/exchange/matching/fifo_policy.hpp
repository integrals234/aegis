#pragma once

#include "cpp/exchange/matching/matching_policy.hpp"

/// Price-time FIFO on `(price_units, priority)` (AEGIS-027, AEGIS-039). Walks
/// the opposite side from its best price outward, and within a level from
/// head (oldest) to tail (newest), stopping as soon as the aggressor's
/// quantity is exhausted or no further level crosses its limit.
namespace aegis::exchange {

class FifoPolicy final : public MatchingPolicy {
 public:
  [[nodiscard]] std::vector<Pairing> match(const OrderBook& book, Side aggressor_side,
                                           std::optional<PriceUnits> limit_price,
                                           QuantityUnits quantity) const override;
};

}  // namespace aegis::exchange
