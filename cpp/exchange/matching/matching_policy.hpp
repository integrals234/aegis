#pragma once

#include <optional>
#include <vector>

#include "cpp/exchange/order_book/book.hpp"
#include "cpp/exchange/order_book/types.hpp"

/// Matching-policy seam (AEGIS-040): the architecture permits later FIFO/
/// pro-rata variants without contaminating the FIFO core. A policy only
/// *decides* which resting orders an aggressor would trade against and how
/// much of each — it never mutates the book. `MatchingEngine` (added when the
/// full accept/reject/trade/terminate orchestration lands) applies the
/// decision: creating trades, decrementing quantities, resting any residual.
namespace aegis::exchange {

/// One resting order this aggressor would trade against, and for how much.
/// Pairings are ordered maker-first-visited-first, i.e. FIFO/price-time order
/// under `FifoPolicy`.
struct Pairing {
  OrderId maker_order_id;
  QuantityUnits fill_quantity;

  friend bool operator==(const Pairing&, const Pairing&) = default;
};

class MatchingPolicy {
 public:
  MatchingPolicy() = default;
  MatchingPolicy(const MatchingPolicy&) = delete;
  MatchingPolicy& operator=(const MatchingPolicy&) = delete;
  MatchingPolicy(MatchingPolicy&&) = delete;
  MatchingPolicy& operator=(MatchingPolicy&&) = delete;
  virtual ~MatchingPolicy() = default;

  /// `limit_price`: the aggressor's limit, or `std::nullopt` for a market
  /// order (no price bound — matches until the book or `quantity` is
  /// exhausted). Does not mutate `book`. Visits only the levels and orders it
  /// consumes (AEGIS-039): a resting order with zero overlap with the
  /// aggressor is never touched.
  [[nodiscard]] virtual std::vector<Pairing> match(const OrderBook& book, Side aggressor_side,
                                                   std::optional<PriceUnits> limit_price,
                                                   QuantityUnits quantity) const = 0;
};

}  // namespace aegis::exchange
