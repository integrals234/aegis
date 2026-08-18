#include "cpp/participant/risk/risk_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "cpp/participant/risk/margin_model.hpp"

namespace aegis::participant::risk {
namespace {

[[nodiscard]] constexpr std::int64_t abs64(std::int64_t value) { return value < 0 ? -value : value; }

[[nodiscard]] std::string dedupe_key(const std::string& strategy_id, const std::string& proposal_id,
                                     std::uint32_t leg_index) {
  return strategy_id + "|" + proposal_id + "|" + std::to_string(leg_index);
}

}  // namespace

std::int64_t RiskEngine::notional_units_base_currency(std::uint32_t instrument_id,
                                                       std::int64_t quantity_units,
                                                       std::int64_t price_units,
                                                       bool& unsupported_currency) const {
  unsupported_currency = false;
  std::int64_t multiplier = 1;
  std::string currency = config_.base_currency;
  if (const auto found = config_.instruments.find(instrument_id); found != config_.instruments.end()) {
    multiplier = found->second.multiplier_units;
    currency = found->second.currency;
  }
  double fx_rate = 1.0;
  if (currency != config_.base_currency) {
    const auto rate = config_.fx_rate_to_base.find(currency);
    if (rate == config_.fx_rate_to_base.end()) {
      unsupported_currency = true;
      return 0;
    }
    fx_rate = rate->second;
  }
  const double raw = static_cast<double>(abs64(quantity_units)) *
                     static_cast<double>(price_units) * static_cast<double>(multiplier) * fx_rate;
  return static_cast<std::int64_t>(raw);
}

std::int64_t RiskEngine::gross_portfolio_notional_units(const EvaluationOverlay& overlay) const {
  std::int64_t total = 0;
  std::unordered_map<std::uint32_t, std::int64_t> exposure = state_.all_positions();
  for (const auto& [instrument_id, reserved] : state_.all_reservations()) {
    exposure[instrument_id] += reserved;
  }
  for (const auto& [instrument_id, extra] : overlay.extra_reserved_by_instrument) {
    exposure[instrument_id] += extra;
  }
  for (const auto& [instrument_id, quantity_units] : exposure) {
    const auto* quote = state_.valid_quote(instrument_id);
    if (quote == nullptr) {
      continue;  // No known price: excluded, not fabricated (docs/LIMITATIONS.md).
    }
    bool unsupported = false;
    total += notional_units_base_currency(instrument_id, quantity_units, quote->reference_price_units,
                                          unsupported);
  }
  return total;
}

std::int64_t RiskEngine::group_gross_notional_units(const std::vector<std::uint32_t>& members,
                                                     const EvaluationOverlay& overlay) const {
  std::int64_t total = 0;
  for (const std::uint32_t instrument_id : members) {
    std::int64_t exposure = state_.position_units(instrument_id) + state_.reserved_units(instrument_id);
    if (const auto found = overlay.extra_reserved_by_instrument.find(instrument_id);
        found != overlay.extra_reserved_by_instrument.end()) {
      exposure += found->second;
    }
    const auto* quote = state_.valid_quote(instrument_id);
    if (quote == nullptr) {
      continue;
    }
    bool unsupported = false;
    total +=
        notional_units_base_currency(instrument_id, exposure, quote->reference_price_units, unsupported);
  }
  return total;
}

double RiskEngine::realized_volatility(std::uint32_t instrument_id) const {
  const auto found = volatility_by_instrument_.find(instrument_id);
  if (found == volatility_by_instrument_.end()) {
    return 0.0;
  }
  return found->second.realized_volatility();
}

LegDecision RiskEngine::evaluate_leg(const OrderRequest& request, const EvaluationOverlay& overlay) const {
  // 1. Halts: kill switches and latched daily-loss/drawdown, checked first --
  // nothing else matters once trading is stopped.
  if (state_.is_globally_halted()) {
    return LegDecision{
        .verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kKillSwitchGlobal,
        .reason = "global kill switch tripped"};
  }
  if (state_.is_strategy_halted(request.strategy_id)) {
    return LegDecision{
        .verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kKillSwitchStrategy,
        .reason = "strategy kill switch tripped: " + request.strategy_id};
  }
  if (state_.is_daily_loss_tripped() || state_.is_drawdown_tripped()) {
    return LegDecision{
        .verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kTradingHalted,
        .reason = "trading halted by daily-loss/drawdown latch"};
  }

  // 2. Connectivity.
  if (!state_.feed_connected()) {
    return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kFeedDisconnected,
                       .reason = "market data feed disconnected"};
  }
  if (!state_.exchange_connected()) {
    return LegDecision{
        .verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kExchangeDisconnected,
        .reason = "exchange connectivity lost"};
  }
  if (!state_.broker_connected()) {
    return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kBrokerDisconnected,
                       .reason = "broker connectivity lost"};
  }

  // 3. Market data validity/staleness. A stale/invalid update never became
  // the new reference (RiskState::note_market_quote only updates
  // last_valid_quote_ when valid), so this reads whatever the last GOOD
  // quote was.
  const auto* quote = state_.valid_quote(request.instrument_id);
  if (quote == nullptr) {
    return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kNoReferencePrice,
                       .reason = "no valid reference price for instrument"};
  }
  if (config_.max_quote_age.nanos() > 0) {
    const common::Nanos age = request.request_time_nanos - quote->observed_at_nanos;
    if (age > config_.max_quote_age.nanos()) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kStaleMarketData,
                         .reason = "market data older than max_quote_age"};
    }
  }

  // 4. Price collar, against the last valid reference.
  if (config_.price_collar_bps > 0 && quote->reference_price_units != 0) {
    const double deviation_bps = 10'000.0 *
        static_cast<double>(std::llabs(request.price_units - quote->reference_price_units)) /
        static_cast<double>(std::llabs(quote->reference_price_units));
    if (deviation_bps > static_cast<double>(config_.price_collar_bps)) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kPriceCollar,
                         .reason = "order price outside collar of reference price"};
    }
  }

  // 5. Idempotency.
  const std::string key = dedupe_key(request.strategy_id, request.proposal_id, request.leg_index);
  if (state_.seen_before(key)) {
    return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kDuplicateRequest,
                       .reason = "duplicate/replayed proposal leg"};
  }

  // 6. Message rate limit (orders).
  if (config_.max_orders_per_window > 0) {
    const std::size_t count = state_.order_count_in_window(request.request_time_nanos,
                                                            config_.rate_limit_window) +
                              overlay.extra_orders_in_window;
    if (count >= config_.max_orders_per_window) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kMessageRateLimit,
                         .reason = "order message rate limit exceeded"};
    }
  }

  // 7. Quantity: order-quantity cap, then volatility-triggered resize.
  std::int64_t effective_quantity = request.quantity_units;
  bool resized = false;
  if (const auto limit = config_.order_quantity_limits.find(request.instrument_id);
      limit != config_.order_quantity_limits.end() &&
      effective_quantity > limit->second.max_order_quantity_units) {
    if (!limit->second.resize_on_breach) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kMaxOrderQuantity,
                         .reason = "order quantity exceeds max_order_quantity"};
    }
    effective_quantity = limit->second.max_order_quantity_units;
    resized = true;
  }
  if (config_.volatility.target_volatility > 0.0) {
    const double realized = realized_volatility(request.instrument_id);
    if (realized > config_.volatility.target_volatility) {
      if (realized >= config_.volatility.target_volatility * config_.volatility.hard_reject_multiple) {
        return LegDecision{
            .verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kVolatilityReduction,
            .reason = "realized volatility beyond hard-reject multiple of target"};
      }
      const double scale = config_.volatility.target_volatility / realized;
      const std::int64_t scaled =
          std::max<std::int64_t>(1, static_cast<std::int64_t>(static_cast<double>(effective_quantity) * scale));
      if (scaled < effective_quantity) {
        effective_quantity = scaled;
        resized = true;
      }
    }
  }

  // 8. Position limits, against the projected position: confirmed + all
  // reservations (including this proposal's prior legs via overlay) + this
  // candidate.
  const std::int64_t signed_candidate =
      request.side == Side::kBuy ? effective_quantity : -effective_quantity;
  std::int64_t projected =
      state_.position_units(request.instrument_id) + state_.reserved_units(request.instrument_id);
  if (const auto extra = overlay.extra_reserved_by_instrument.find(request.instrument_id);
      extra != overlay.extra_reserved_by_instrument.end()) {
    projected += extra->second;
  }
  projected += signed_candidate;
  if (const auto limit = config_.position_limits.find(request.instrument_id);
      limit != config_.position_limits.end()) {
    if (projected > limit->second.max_long_units) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kMaxPositionLong,
                         .reason = "projected long position exceeds max_long_units"};
    }
    if (-projected > limit->second.max_short_units) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kMaxPositionShort,
                         .reason = "projected short position exceeds max_short_units"};
    }
  }

  // 9. Notional: per-order, then portfolio.
  bool unsupported_currency = false;
  const std::int64_t order_notional = notional_units_base_currency(
      request.instrument_id, effective_quantity, request.price_units, unsupported_currency);
  if (unsupported_currency) {
    return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kUnsupportedCurrency,
                       .reason = "instrument currency has no configured FX rate to base_currency"};
  }
  if (config_.max_order_notional_units > 0 && order_notional > config_.max_order_notional_units) {
    return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kMaxOrderNotional,
                       .reason = "order notional exceeds max_order_notional_units"};
  }
  EvaluationOverlay portfolio_overlay = overlay;
  portfolio_overlay.extra_reserved_by_instrument[request.instrument_id] += signed_candidate;
  const std::int64_t portfolio_notional = gross_portfolio_notional_units(portfolio_overlay);
  if (config_.max_portfolio_notional_units > 0 &&
      portfolio_notional > config_.max_portfolio_notional_units) {
    return LegDecision{
        .verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kMaxPortfolioNotional,
        .reason = "portfolio notional exceeds max_portfolio_notional_units"};
  }

  // 10. Market/sector exposure.
  if (const auto info = config_.instruments.find(request.instrument_id); info != config_.instruments.end()) {
    if (!info->second.market.empty()) {
      if (const auto limit = config_.market_exposure_limit_units.find(info->second.market);
          limit != config_.market_exposure_limit_units.end()) {
        // Approximation: the market group's total is this instrument's own
        // exposure only, unless config groups multiple instruments under
        // the same market name via `instruments[].market` -- the sum below
        // walks every configured instrument sharing that market string.
        std::vector<std::uint32_t> members;
        for (const auto& [other_id, other_info] : config_.instruments) {
          if (other_info.market == info->second.market) {
            members.push_back(other_id);
          }
        }
        if (group_gross_notional_units(members, portfolio_overlay) > limit->second) {
          return LegDecision{
              .verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kMarketExposure,
              .reason = "market exposure exceeds configured limit"};
        }
      }
    }
    if (!info->second.sector.empty()) {
      if (const auto limit = config_.sector_exposure_limit_units.find(info->second.sector);
          limit != config_.sector_exposure_limit_units.end()) {
        std::vector<std::uint32_t> members;
        for (const auto& [other_id, other_info] : config_.instruments) {
          if (other_info.sector == info->second.sector) {
            members.push_back(other_id);
          }
        }
        if (group_gross_notional_units(members, portfolio_overlay) > limit->second) {
          return LegDecision{
              .verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kSectorExposure,
              .reason = "sector exposure exceeds configured limit"};
        }
      }
    }
  }

  // 11. Concentration and correlated-group exposure.
  if (config_.concentration.max_concentration_share < 1.0 && portfolio_notional > 0) {
    const auto* quote_for_candidate = state_.valid_quote(request.instrument_id);
    std::int64_t instrument_exposure =
        state_.position_units(request.instrument_id) + state_.reserved_units(request.instrument_id) +
        signed_candidate;
    bool unsupported = false;
    const std::int64_t instrument_notional =
        quote_for_candidate == nullptr
            ? 0
            : notional_units_base_currency(request.instrument_id, instrument_exposure,
                                           quote_for_candidate->reference_price_units, unsupported);
    if (static_cast<double>(instrument_notional) / static_cast<double>(portfolio_notional) >
        config_.concentration.max_concentration_share) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kConcentration,
                         .reason = "instrument concentration exceeds max_concentration_share"};
    }
  }
  for (const auto& [group_id, members] : config_.concentration.correlated_groups) {
    if (std::ranges::find(members, request.instrument_id) == members.end()) {
      continue;
    }
    const auto limit = config_.concentration.group_exposure_limit_units.find(group_id);
    if (limit == config_.concentration.group_exposure_limit_units.end()) {
      continue;
    }
    if (group_gross_notional_units(members, portfolio_overlay) > limit->second) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kCorrelatedExposure,
                         .reason = "correlated group exposure exceeds configured limit: " + group_id};
    }
  }

  // 12. Margin and leverage, using equity fed via on_equity_update.
  std::unordered_map<std::uint32_t, std::int64_t> exposure_for_margin = state_.all_positions();
  for (const auto& [instrument_id, reserved] : state_.all_reservations()) {
    exposure_for_margin[instrument_id] += reserved;
  }
  for (const auto& [instrument_id, extra] : portfolio_overlay.extra_reserved_by_instrument) {
    exposure_for_margin[instrument_id] += extra;
  }
  const std::int64_t required_margin = total_required_margin_units(config_.margin, exposure_for_margin);
  if (!config_.margin.margin_per_contract_units.empty() && required_margin > state_.equity_units()) {
    return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kInsufficientMargin,
                       .reason = "required margin exceeds available equity"};
  }
  if (config_.max_leverage > 0.0) {
    if (state_.equity_units() <= 0 && portfolio_notional > 0) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kMaxLeverage,
                         .reason = "non-positive equity cannot support any leverage"};
    }
    if (state_.equity_units() > 0 &&
        static_cast<double>(portfolio_notional) / static_cast<double>(state_.equity_units()) >
            config_.max_leverage) {
      return LegDecision{.verdict = RiskVerdict::kReject, .reason_code = ReasonCode::kMaxLeverage,
                         .reason = "gross notional / equity exceeds max_leverage"};
    }
  }

  return LegDecision{
      .verdict = resized ? RiskVerdict::kResize : RiskVerdict::kApprove,
      .approved_quantity_units = effective_quantity,
      .reason_code = ReasonCode::kNone,
      .reason = "",
  };
}

LegDecision RiskEngine::evaluate(const OrderRequest& request) const {
  return evaluate_leg(request, EvaluationOverlay{});
}

ProposalDecisionResult RiskEngine::evaluate_proposal(const std::string& strategy_id,
                                                      const std::string& proposal_id,
                                                      const std::vector<OrderRequest>& legs) const {
  EvaluationOverlay overlay;
  std::vector<LegDecision> decisions;
  decisions.reserve(legs.size());
  for (const OrderRequest& raw_leg : legs) {
    OrderRequest leg = raw_leg;
    leg.strategy_id = strategy_id;
    leg.proposal_id = proposal_id;
    const LegDecision decision = evaluate_leg(leg, overlay);
    if (decision.verdict == RiskVerdict::kReject) {
      // Atomicity (ADR-0027): the first rejecting leg makes the WHOLE
      // proposal reject; every leg reports kReject, not just this one --
      // a reader must never see one leg's decision imply the sibling leg
      // was independently fine.
      std::vector<LegDecision> all_rejected(legs.size(),
                                            LegDecision{.verdict = RiskVerdict::kReject,
                                                       .reason_code = decision.reason_code,
                                                       .reason = decision.reason});
      return ProposalDecisionResult{.verdict = RiskVerdict::kReject,
                                    .reason_code = decision.reason_code,
                                    .reason = decision.reason,
                                    .legs = std::move(all_rejected)};
    }
    decisions.push_back(decision);
    const std::int64_t signed_qty =
        leg.side == Side::kBuy ? decision.approved_quantity_units : -decision.approved_quantity_units;
    overlay.extra_reserved_by_instrument[leg.instrument_id] += signed_qty;
    overlay.extra_orders_in_window += 1;
  }
  const bool any_resize =
      std::ranges::any_of(decisions, [](const LegDecision& d) { return d.verdict == RiskVerdict::kResize; });
  return ProposalDecisionResult{
      .verdict = any_resize ? RiskVerdict::kResize : RiskVerdict::kApprove,
      .reason_code = ReasonCode::kNone,
      .reason = "",
      .legs = std::move(decisions),
  };
}

const ProposalRiskDecision& RiskEngine::commit_proposal_decision(const std::string& strategy_id,
                                                                  const std::string& proposal_id,
                                                                  const std::vector<OrderRequest>& legs,
                                                                  common::Nanos decided_at_nanos) {
  const ProposalDecisionResult result = evaluate_proposal(strategy_id, proposal_id, legs);

  if (result.verdict != RiskVerdict::kReject) {
    // Commit side effects only now -- rate-limit tokens, dedupe keys and
    // pending-leg arming happen exactly once, after the pure preflight
    // above has already proven the whole proposal is admissible.
    for (std::size_t index = 0; index < legs.size(); ++index) {
      const OrderRequest& leg = legs[index];
      const LegDecision& decision = result.legs[index];
      state_.mark_seen(dedupe_key(strategy_id, proposal_id, leg.leg_index));
      state_.record_order_event(leg.request_time_nanos);
      pending_legs_.push_back(PendingLeg{
          .strategy_id = strategy_id,
          .proposal_id = proposal_id,
          .leg_index = leg.leg_index,
          .instrument_id = leg.instrument_id,
          .side = leg.side,
          .requested_quantity_units = leg.quantity_units,
          .approved_quantity_units = decision.approved_quantity_units,
          .verdict = decision.verdict,
          .reason_code = decision.reason_code,
          .reason = decision.reason,
      });
    }
  }

  return audit_log_.record_proposal(strategy_id, proposal_id, result.verdict, result.reason_code,
                                    result.reason, result.legs, decided_at_nanos);
}

OmsDecision RiskEngine::decide_order(std::uint32_t instrument_id, Side side,
                                     std::int64_t quantity_units, std::uint64_t client_order_id,
                                     common::Nanos now_nanos) {
  const auto found = std::ranges::find_if(pending_legs_, [&](const PendingLeg& pending) {
    return pending.instrument_id == instrument_id && pending.side == side &&
           pending.requested_quantity_units == quantity_units;
  });
  if (found == pending_legs_.end()) {
    audit_log_.record_order("", 0, client_order_id, instrument_id, RiskVerdict::kReject, quantity_units,
                            0, ReasonCode::kUnexpectedOrder, "no armed proposal decision for this order",
                            now_nanos);
    return OmsDecision{.verdict = RiskVerdict::kReject,
                       .approved_quantity_units = 0,
                       .reason_code = ReasonCode::kUnexpectedOrder,
                       .reason = "no armed proposal decision for this order"};
  }
  PendingLeg pending = *found;
  pending_legs_.erase(found);

  // A halt that tripped after commit_proposal_decision but before this order
  // actually reached the OMS still stops it here -- the seam is the last
  // point before adapter_->submit(), so it is where a late-breaking halt
  // must actually bite.
  RiskVerdict verdict = pending.verdict;
  ReasonCode reason_code = pending.reason_code;
  std::string reason = pending.reason;
  std::int64_t approved = pending.approved_quantity_units;
  if (state_.is_globally_halted()) {
    verdict = RiskVerdict::kReject;
    reason_code = ReasonCode::kKillSwitchGlobal;
    reason = "global kill switch tripped after proposal decision";
    approved = 0;
  } else if (state_.is_strategy_halted(pending.strategy_id)) {
    verdict = RiskVerdict::kReject;
    reason_code = ReasonCode::kKillSwitchStrategy;
    reason = "strategy kill switch tripped after proposal decision";
    approved = 0;
  }

  if (verdict != RiskVerdict::kReject) {
    state_.reserve(client_order_id, instrument_id, side, approved, pending.strategy_id);
  }

  audit_log_.record_order(pending.proposal_id, pending.leg_index, client_order_id, instrument_id, verdict,
                          quantity_units, approved, reason_code, reason, now_nanos);
  return OmsDecision{.verdict = verdict,
                     .approved_quantity_units = approved,
                     .reason_code = reason_code,
                     .reason = reason};
}

bool RiskEngine::allow_cancel(common::Nanos now_nanos, bool bypass_safety) {
  if (bypass_safety) {
    return true;
  }
  if (config_.max_cancels_per_window == 0) {
    return true;
  }
  if (state_.cancel_count_in_window(now_nanos, config_.rate_limit_window) >=
      config_.max_cancels_per_window) {
    return false;
  }
  state_.record_cancel_event(now_nanos);
  return true;
}

void RiskEngine::on_fill(std::uint64_t client_order_id, std::uint32_t instrument_id, Side side,
                         std::int64_t fill_quantity_units) {
  state_.apply_fill(instrument_id, side, fill_quantity_units);
  state_.reduce_reservation(client_order_id, fill_quantity_units);
}

void RiskEngine::on_order_terminated(std::uint64_t client_order_id) {
  state_.release_reservation(client_order_id);
}

void RiskEngine::on_order_rejected(std::uint64_t client_order_id) {
  state_.release_reservation(client_order_id);
}

void RiskEngine::on_market_data(std::uint32_t instrument_id, std::int64_t reference_price_units,
                                common::Nanos observed_at_nanos, bool valid) {
  if (valid && reference_price_units > 0) {
    const auto last = last_price_by_instrument_.find(instrument_id);
    if (last != last_price_by_instrument_.end() && last->second > 0) {
      const double return_value =
          (static_cast<double>(reference_price_units) - static_cast<double>(last->second)) /
          static_cast<double>(last->second);
      auto [iterator, inserted] = volatility_by_instrument_.try_emplace(
          instrument_id, config_.volatility.window > 0 ? config_.volatility.window : 20);
      iterator->second.push(return_value);
    }
    last_price_by_instrument_[instrument_id] = reference_price_units;
  }
  state_.note_market_quote(MarketQuote{.instrument_id = instrument_id,
                                       .reference_price_units = reference_price_units,
                                       .observed_at_nanos = observed_at_nanos,
                                       .valid = valid && reference_price_units > 0});
}

void RiskEngine::on_equity_update(std::int64_t equity_units) {
  state_.set_equity_units(equity_units);
  drawdown_.push(static_cast<double>(equity_units));
  if (config_.max_drawdown_units > 0 &&
      drawdown_.current_drawdown() > static_cast<double>(config_.max_drawdown_units)) {
    state_.trip_drawdown();
  }
}

void RiskEngine::on_session_pnl_update(std::int64_t cumulative_session_pnl_units) {
  if (config_.daily_loss_limit_units > 0 &&
      cumulative_session_pnl_units <= -config_.daily_loss_limit_units) {
    state_.trip_daily_loss();
  }
}

}  // namespace aegis::participant::risk
