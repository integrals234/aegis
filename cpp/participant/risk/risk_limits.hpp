#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/common/time.hpp"

/// M5's risk configuration (ADR-0028). Every field here is a documented,
/// deliberately simplified control, not a claim of production adequacy --
/// see `docs/LIMITATIONS.md`'s M5 section.
namespace aegis::participant::risk {

/// AEGIS-121: per-instrument order-quantity cap. `resize_on_breach == true`
/// caps the order at the limit instead of rejecting it outright -- both
/// branches of the frozen "reject or resize" acceptance are real config, not
/// a hardcoded choice.
struct OrderQuantityLimit {
  std::int64_t max_order_quantity_units{0};
  bool resize_on_breach{false};
};

/// AEGIS-122: asymmetric long/short position caps, checked against the
/// *projected* position -- current filled position plus every reserved,
/// risk-approved-but-not-yet-terminal order plus this candidate.
struct PositionLimit {
  std::int64_t max_long_units{0};
  std::int64_t max_short_units{0};  ///< Stored positive; compared to abs(short exposure).
};

/// AEGIS-123: instrument identity for notional/currency normalization.
/// `multiplier_units` mirrors `configs/futures/products.yaml`'s decimal
/// `multiplier` field, as a whole-number contract multiplier (every in-repo
/// product's multiplier is an integer: 50, 1000, 2500).
struct InstrumentInfo {
  std::int64_t multiplier_units{1};
  std::string currency{"USD"};
  std::string market{};  ///< AEGIS-124 grouping key, e.g. product_root.
  std::string sector{};  ///< AEGIS-124 grouping key.
};

/// AEGIS-129 (ADR-0028 Model A): `margin_per_contract_units * abs(quantity)`.
/// `margin_per_contract_units` is currency per contract, not a rate -- no
/// multiplier or reference price enters this formula, so its units are
/// unambiguous by construction. NOT SPAN, NOT an exchange clearing model,
/// NOT a claim of production margin adequacy (`docs/LIMITATIONS.md`).
struct MarginConfig {
  std::unordered_map<std::uint32_t, std::int64_t> margin_per_contract_units;
};

/// AEGIS-133: as realized volatility rises above `target_volatility`, the
/// approved quantity shrinks proportionally
/// (`requested * target_volatility / realized_volatility`, floored at 1
/// unit); at or beyond `hard_reject_multiple * target_volatility` the whole
/// order is rejected rather than resized to a token size.
struct VolatilityReductionConfig {
  std::size_t window{20};
  double target_volatility{0.0};
  double hard_reject_multiple{3.0};
};

/// AEGIS-134: concentration is a single instrument's share of gross
/// portfolio notional; "correlated exposure" is a config-supplied grouping
/// standing in for a correlation matrix, deliberately never estimated in the
/// decision path (ADR-0028) -- estimating it online would make a risk
/// decision depend on a statistic computed from the same data the decision
/// is about, an unreviewable feedback loop this project avoids.
struct ConcentrationConfig {
  double max_concentration_share{1.0};  ///< 1.0 == no concentration limit.
  std::unordered_map<std::string, std::vector<std::uint32_t>> correlated_groups;
  std::unordered_map<std::string, std::int64_t> group_exposure_limit_units;
};

struct RiskLimitsConfig {
  std::unordered_map<std::uint32_t, OrderQuantityLimit> order_quantity_limits;
  std::unordered_map<std::uint32_t, PositionLimit> position_limits;

  std::int64_t max_order_notional_units{0};      ///< 0 == unlimited.
  std::int64_t max_portfolio_notional_units{0};  ///< 0 == unlimited.
  std::string base_currency{"USD"};
  /// FX rate to `base_currency`; a currency absent here is unsupported and
  /// rejected explicitly (AEGIS-123's "or unsupported scope is explicit").
  std::unordered_map<std::string, double> fx_rate_to_base;

  std::unordered_map<std::uint32_t, InstrumentInfo> instruments;
  std::unordered_map<std::string, std::int64_t> market_exposure_limit_units;
  std::unordered_map<std::string, std::int64_t> sector_exposure_limit_units;

  std::int64_t price_collar_bps{0};  ///< 0 == no collar.

  common::Duration max_quote_age{common::Duration{0}};  ///< 0 == staleness check disabled.

  common::Duration rate_limit_window{common::Duration{1'000'000'000}};  ///< 1s default.
  std::uint32_t max_orders_per_window{0};   ///< 0 == unlimited.
  std::uint32_t max_cancels_per_window{0};  ///< 0 == unlimited.

  MarginConfig margin;
  double max_leverage{0.0};  ///< 0.0 == unlimited.

  std::int64_t daily_loss_limit_units{0};  ///< 0 == unlimited. Positive threshold on realized loss.
  std::int64_t max_drawdown_units{0};      ///< 0 == unlimited.

  VolatilityReductionConfig volatility;
  ConcentrationConfig concentration;
};

}  // namespace aegis::participant::risk
