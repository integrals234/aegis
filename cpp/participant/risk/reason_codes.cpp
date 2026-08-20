#include "cpp/participant/risk/reason_codes.hpp"

namespace aegis::participant::risk {

std::string_view describe(ReasonCode reason) {
  switch (reason) {
    case ReasonCode::kNone:
      return "none";
    case ReasonCode::kMaxOrderQuantity:
      return "max_order_quantity";
    case ReasonCode::kMaxPositionLong:
      return "max_position_long";
    case ReasonCode::kMaxPositionShort:
      return "max_position_short";
    case ReasonCode::kMaxOrderNotional:
      return "max_order_notional";
    case ReasonCode::kMaxPortfolioNotional:
      return "max_portfolio_notional";
    case ReasonCode::kUnsupportedCurrency:
      return "unsupported_currency";
    case ReasonCode::kMarketExposure:
      return "market_exposure";
    case ReasonCode::kSectorExposure:
      return "sector_exposure";
    case ReasonCode::kPriceCollar:
      return "price_collar";
    case ReasonCode::kNoReferencePrice:
      return "no_reference_price";
    case ReasonCode::kStaleMarketData:
      return "stale_market_data";
    case ReasonCode::kInvalidMarketData:
      return "invalid_market_data";
    case ReasonCode::kDuplicateRequest:
      return "duplicate_request";
    case ReasonCode::kMessageRateLimit:
      return "message_rate_limit";
    case ReasonCode::kInsufficientMargin:
      return "insufficient_margin";
    case ReasonCode::kMaxLeverage:
      return "max_leverage";
    case ReasonCode::kDailyLossLimit:
      return "daily_loss_limit";
    case ReasonCode::kMaxDrawdown:
      return "max_drawdown";
    case ReasonCode::kVolatilityReduction:
      return "volatility_reduction";
    case ReasonCode::kConcentration:
      return "concentration";
    case ReasonCode::kCorrelatedExposure:
      return "correlated_exposure";
    case ReasonCode::kKillSwitchStrategy:
      return "kill_switch_strategy";
    case ReasonCode::kKillSwitchGlobal:
      return "kill_switch_global";
    case ReasonCode::kTradingHalted:
      return "trading_halted";
    case ReasonCode::kFeedDisconnected:
      return "feed_disconnected";
    case ReasonCode::kExchangeDisconnected:
      return "exchange_disconnected";
    case ReasonCode::kBrokerDisconnected:
      return "broker_disconnected";
    case ReasonCode::kUnexpectedOrder:
      return "unexpected_order";
    case ReasonCode::kIdentityMismatch:
      return "identity_mismatch";
    case ReasonCode::kProposalNotAuthorized:
      return "proposal_not_authorized";
    case ReasonCode::kIncompleteProposalStaging:
      return "incomplete_proposal_staging";
  }
  return "unknown";
}

}  // namespace aegis::participant::risk
