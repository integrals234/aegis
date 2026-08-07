#pragma once

#include <map>
#include <optional>

#include "cpp/exchange/order_book/level.hpp"
#include "cpp/exchange/order_book/types.hpp"

/// Price-level index (AEGIS-038, AEGIS-040).
///
/// `LevelIndex` is an interface over the sorted level container so a later
/// milestone can substitute a tick array or bitset without touching matching
/// (AEGIS-042/043, M8). `MapLevelIndex` — `std::map`, descending for bids,
/// ascending for asks — is the M1 baseline.
///
/// Documented complexity: expected O(log D) insert/find/erase, where D is
/// `InstrumentSpec::price_domain_size()` — the finite, documented price-domain
/// cardinality AEGIS-038 requires. `configs/claims_policy.yaml` bans "all
/// operations strict O(1)"; this is not that claim.
namespace aegis::exchange {

class LevelIndex {
 public:
  LevelIndex() = default;
  LevelIndex(const LevelIndex&) = delete;
  LevelIndex& operator=(const LevelIndex&) = delete;
  LevelIndex(LevelIndex&&) = delete;
  LevelIndex& operator=(LevelIndex&&) = delete;
  virtual ~LevelIndex() = default;

  /// Returns the level at `price`, creating an empty one if absent. Not
  /// `[[nodiscard]]`: callers legitimately invoke this for its side effect
  /// (ensuring the level exists) without using the reference, e.g. warming a
  /// price grid ahead of a benchmark.
  virtual PriceLevel& level_at(PriceUnits price) = 0;
  [[nodiscard]] virtual PriceLevel* find(PriceUnits price) = 0;
  [[nodiscard]] virtual const PriceLevel* find(PriceUnits price) const = 0;
  /// Precondition: the level at `price` exists and is empty. Erasing a
  /// nonempty level would silently drop resting orders from the index while
  /// `OrderStorage` still holds them live — the bijection invariant
  /// (`InvariantScope::kStructural`) exists to catch exactly that.
  virtual void erase(PriceUnits price) = 0;
  /// The best (highest bid / lowest ask) price with a nonempty level, if any.
  [[nodiscard]] virtual std::optional<PriceUnits> best_price() const = 0;
  /// The next price strictly worse than `price` (i.e. the level a multi-level
  /// sweep would visit after exhausting `price`), if any. Lets matching walk
  /// levels one at a time without materializing the whole side — the
  /// output-sensitive bound AEGIS-039 requires.
  [[nodiscard]] virtual std::optional<PriceUnits> next_price_after(PriceUnits price) const = 0;
  [[nodiscard]] virtual bool empty() const = 0;
};

/// Orders bids highest-first and asks lowest-first via the same class, driven
/// by the comparator supplied at construction — one implementation for both
/// sides rather than a near-duplicate pair.
class MapLevelIndex final : public LevelIndex {
 public:
  /// `descending`: true for bids (best = highest price), false for asks
  /// (best = lowest price).
  explicit MapLevelIndex(bool descending) : levels_(PriceOrder{descending}) {}

  PriceLevel& level_at(PriceUnits price) override;
  [[nodiscard]] PriceLevel* find(PriceUnits price) override;
  [[nodiscard]] const PriceLevel* find(PriceUnits price) const override;
  void erase(PriceUnits price) override;
  [[nodiscard]] std::optional<PriceUnits> best_price() const override;
  [[nodiscard]] std::optional<PriceUnits> next_price_after(PriceUnits price) const override;
  [[nodiscard]] bool empty() const override;

  [[nodiscard]] std::size_t level_count() const { return levels_.size(); }

 private:
  struct PriceOrder {
    bool descending;
    [[nodiscard]] bool operator()(const PriceUnits& a, const PriceUnits& b) const {
      return descending ? (b < a) : (a < b);
    }
  };

  std::map<PriceUnits, PriceLevel, PriceOrder> levels_;
};

}  // namespace aegis::exchange
