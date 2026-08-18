#pragma once

#include <cstdint>
#include <string_view>

/// Closed set of risk decision reasons (AEGIS-137). Every `RiskVerdict` other
/// than a plain approve carries exactly one of these -- the audit record's
/// `reason_code` is this enum, never a free-text-only explanation.
namespace aegis::participant::risk {

/// NOLINTNEXTLINE(performance-enum-size)
enum class ReasonCode : std::uint8_t {
  kNone = 0,  ///< Approved outright; no limit was binding.
  kMaxOrderQuantity = 1,
  kMaxPositionLong = 2,
  kMaxPositionShort = 3,
  kMaxOrderNotional = 4,
  kMaxPortfolioNotional = 5,
  kUnsupportedCurrency = 6,
  kMarketExposure = 7,
  kSectorExposure = 8,
  kPriceCollar = 9,
  kNoReferencePrice = 10,
  kStaleMarketData = 11,
  kInvalidMarketData = 12,
  kDuplicateRequest = 13,
  kMessageRateLimit = 14,
  kInsufficientMargin = 15,
  kMaxLeverage = 16,
  kDailyLossLimit = 17,
  kMaxDrawdown = 18,
  kVolatilityReduction = 19,
  kConcentration = 20,
  kCorrelatedExposure = 21,
  kKillSwitchStrategy = 22,
  kKillSwitchGlobal = 23,
  kTradingHalted = 24,  ///< A halt (daily loss / drawdown / kill switch) already latched.
  kFeedDisconnected = 25,
  kExchangeDisconnected = 26,
  kBrokerDisconnected = 27,
  kUnexpectedOrder = 28,  ///< Seam saw an order no proposal-level decision armed (safety net).
};

[[nodiscard]] std::string_view describe(ReasonCode reason);

}  // namespace aegis::participant::risk
