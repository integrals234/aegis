#include "cpp/participant/risk/risk_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "cpp/participant/risk/margin_model.hpp"

namespace aegis::participant::risk {
namespace {

[[nodiscard]] constexpr std::int64_t abs64(std::int64_t value) {
  return value < 0 ? -value : value;
}

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
  if (const auto found = config_.instruments.find(instrument_id);
      found != config_.instruments.end()) {
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
  const double raw = static_cast<double>(abs64(quantity_units)) * static_cast<double>(price_units) *
                     static_cast<double>(multiplier) * fx_rate;
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
    total += notional_units_base_currency(instrument_id, quantity_units,
                                          quote->reference_price_units, unsupported);
  }
  return total;
}

std::int64_t RiskEngine::group_gross_notional_units(const std::vector<std::uint32_t>& members,
                                                    const EvaluationOverlay& overlay) const {
  std::int64_t total = 0;
  for (const std::uint32_t instrument_id : members) {
    std::int64_t exposure =
        state_.position_units(instrument_id) + state_.reserved_units(instrument_id);
    if (const auto found = overlay.extra_reserved_by_instrument.find(instrument_id);
        found != overlay.extra_reserved_by_instrument.end()) {
      exposure += found->second;
    }
    const auto* quote = state_.valid_quote(instrument_id);
    if (quote == nullptr) {
      continue;
    }
    bool unsupported = false;
    total += notional_units_base_currency(instrument_id, exposure, quote->reference_price_units,
                                          unsupported);
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

/// Control group 0 (M5 closure repair, R1): quantity is a positive
/// MAGNITUDE; `Side` alone carries direction. A negative or zero quantity is
/// malformed input -- never a valid same-magnitude order on the opposite
/// side -- so it is rejected here, before any other control reads
/// `request.quantity_units` at all (a sign-sensitive position/notional
/// projection, or a reservation, treating a negative magnitude as
/// legitimate is exactly how an attacker manufactures a negative reserved
/// quantity that then SUBTRACTS from every cumulative control's projected
/// exposure).
std::optional<LegDecision> RiskEngine::check_quantity_validity(const OrderRequest& request) {
  if (request.quantity_units <= 0) {
    return LegDecision{
        .verdict = RiskVerdict::kReject,
        .reason_code = ReasonCode::kInvalidQuantity,
        .reason = "quantity_units must be a positive magnitude; side carries direction"};
  }
  return std::nullopt;
}

/// Control group 1-2 (ADR-0028): kill switches, latched daily-loss/drawdown,
/// and the three connectivity signals. An engaged optional is a rejection;
/// a disengaged one means "nothing here objected, keep going". Split out of
/// `evaluate_leg` so each group stays independently readable -- the ordering
/// between groups is itself part of the documented semantics (halts first,
/// nothing else matters once trading is stopped).
std::optional<LegDecision> RiskEngine::check_halts_and_connectivity(
    const OrderRequest& request) const {
  // 1. Halts: kill switches and latched daily-loss/drawdown, checked first --
  // nothing else matters once trading is stopped.
  if (state_.is_globally_halted()) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kKillSwitchGlobal,
                       .reason = "global kill switch tripped"};
  }
  if (state_.is_strategy_halted(request.strategy_id)) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kKillSwitchStrategy,
                       .reason = "strategy kill switch tripped: " + request.strategy_id};
  }
  if (state_.is_daily_loss_tripped() || state_.is_drawdown_tripped()) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kTradingHalted,
                       .reason = "trading halted by daily-loss/drawdown latch"};
  }

  // 2. Connectivity.
  if (!state_.feed_connected()) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kFeedDisconnected,
                       .reason = "market data feed disconnected"};
  }
  if (!state_.exchange_connected()) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kExchangeDisconnected,
                       .reason = "exchange connectivity lost"};
  }
  if (!state_.broker_connected()) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kBrokerDisconnected,
                       .reason = "broker connectivity lost"};
  }
  return std::nullopt;
}

/// Control group 3-4: market-data validity/staleness and the price collar,
/// both judged against the last VALID reference quote. Returns that quote
/// through `quote_out` for the caller's later use so it is looked up once.
std::optional<LegDecision> RiskEngine::check_market_state(const OrderRequest& request,
                                                          const MarketQuote*& quote_out) const {
  // 3. Market data validity/staleness. A stale/invalid update never became
  // the new reference (RiskState::note_market_quote only updates
  // last_valid_quote_ when valid), so this reads whatever the last GOOD
  // quote was.
  const auto* quote = state_.valid_quote(request.instrument_id);
  if (quote == nullptr) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kNoReferencePrice,
                       .reason = "no valid reference price for instrument"};
  }
  if (config_.max_quote_age.nanos() > 0) {
    const common::Nanos age = request.request_time_nanos - quote->observed_at_nanos;
    if (age > config_.max_quote_age.nanos()) {
      return LegDecision{.verdict = RiskVerdict::kReject,
                         .reason_code = ReasonCode::kStaleMarketData,
                         .reason = "market data older than max_quote_age"};
    }
  }

  // 4. Price collar, against the last valid reference.
  if (config_.price_collar_bps > 0 && quote->reference_price_units != 0) {
    const double deviation_bps =
        10'000.0 *
        static_cast<double>(std::llabs(request.price_units - quote->reference_price_units)) /
        static_cast<double>(std::llabs(quote->reference_price_units));
    if (deviation_bps > static_cast<double>(config_.price_collar_bps)) {
      return LegDecision{.verdict = RiskVerdict::kReject,
                         .reason_code = ReasonCode::kPriceCollar,
                         .reason = "order price outside collar of reference price"};
    }
  }
  quote_out = quote;
  return std::nullopt;
}

/// Control group 5-6: idempotency and the order message-rate limit.
std::optional<LegDecision> RiskEngine::check_request_admission(
    const OrderRequest& request, const EvaluationOverlay& overlay) const {
  // 5. Idempotency.
  const std::string key = dedupe_key(request.strategy_id, request.proposal_id, request.leg_index);
  if (state_.seen_before(key)) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kDuplicateRequest,
                       .reason = "duplicate/replayed proposal leg"};
  }

  // 6. Message rate limit (orders).
  if (config_.max_orders_per_window > 0) {
    const std::size_t count =
        state_.order_count_in_window(request.request_time_nanos, config_.rate_limit_window) +
        overlay.extra_orders_in_window;
    if (count >= config_.max_orders_per_window) {
      return LegDecision{.verdict = RiskVerdict::kReject,
                         .reason_code = ReasonCode::kMessageRateLimit,
                         .reason = "order message rate limit exceeded"};
    }
  }
  return std::nullopt;
}

/// Control group 7: the order-quantity cap and volatility-triggered
/// resizing. On success writes the approved quantity and whether it was
/// reduced; on failure returns the rejection.
std::optional<LegDecision> RiskEngine::resolve_effective_quantity(const OrderRequest& request,
                                                                  std::int64_t& effective_quantity,
                                                                  bool& resized) const {
  // 7. Quantity: order-quantity cap, then volatility-triggered resize.
  effective_quantity = request.quantity_units;
  resized = false;
  if (const auto limit = config_.order_quantity_limits.find(request.instrument_id);
      limit != config_.order_quantity_limits.end() &&
      effective_quantity > limit->second.max_order_quantity_units) {
    if (!limit->second.resize_on_breach) {
      return LegDecision{.verdict = RiskVerdict::kReject,
                         .reason_code = ReasonCode::kMaxOrderQuantity,
                         .reason = "order quantity exceeds max_order_quantity"};
    }
    effective_quantity = limit->second.max_order_quantity_units;
    resized = true;
  }
  if (config_.volatility.target_volatility > 0.0) {
    const double realized = realized_volatility(request.instrument_id);
    if (realized > config_.volatility.target_volatility) {
      if (realized >=
          config_.volatility.target_volatility * config_.volatility.hard_reject_multiple) {
        return LegDecision{.verdict = RiskVerdict::kReject,
                           .reason_code = ReasonCode::kVolatilityReduction,
                           .reason = "realized volatility beyond hard-reject multiple of target"};
      }
      const double scale = config_.volatility.target_volatility / realized;
      const std::int64_t scaled = std::max<std::int64_t>(
          1, static_cast<std::int64_t>(static_cast<double>(effective_quantity) * scale));
      if (scaled < effective_quantity) {
        effective_quantity = scaled;
        resized = true;
      }
    }
  }
  return std::nullopt;
}

/// Defense-in-depth postcondition (M5 closure repair, N2): whatever
/// `resolve_effective_quantity` computed, the quantity `RiskEngine` is about
/// to reserve and approve MUST be a positive magnitude. `check_quantity_validity`
/// already guarantees the REQUESTED quantity is positive, but a malformed
/// configured limit (a non-positive `max_order_quantity_units` with
/// `resize_on_breach == true`) can still make `resolve_effective_quantity`
/// itself manufacture a non-positive `effective_quantity` -- `RiskEngine`
/// must never reserve or approve that, regardless of how the config got
/// that way (`app::load_risk_limits_config` also rejects it at load time,
/// but this check does not rely on that being the only construction path).
std::optional<LegDecision> RiskEngine::check_approved_quantity_postcondition(
    std::int64_t effective_quantity) {
  if (effective_quantity <= 0) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kInvalidLimitConfiguration,
                       .reason =
                           "computed approved quantity is not a positive magnitude; "
                           "check configured order-quantity/volatility limits"};
  }
  return std::nullopt;
}

/// The volatility HARD-REJECT safety gate only (M5 closure repair, R6) --
/// deliberately excludes the RESIZE branch above, which stays frozen once a
/// proposal is committed. Shares the exact hard-reject THRESHOLD
/// `resolve_effective_quantity` uses, so for the same volatility reading the
/// two agree on WHETHER the threshold is breached.
///
/// Correction (M5 closure repair, N7): this does NOT mean a fresh check at
/// release can never disagree with commit-time sizing. `resolve_effective_quantity`
/// only evaluates its hard-reject branch inside `realized > target_volatility`
/// (`RiskEngine::resolve_effective_quantity`), so with a misconfigured
/// `hard_reject_multiple < 1.0` the hard-reject threshold sits BELOW the
/// resize threshold and commit-time sizing can approve a quantity this
/// function immediately rejects at release, with volatility UNCHANGED in
/// between. See `VolatilityReductionConfig`'s own doc for the intended
/// config domain (`hard_reject_multiple >= 1.0`), which is not validated at
/// load time because no frozen requirement constrains it.
std::optional<LegDecision> RiskEngine::check_volatility_hard_reject(
    std::uint32_t instrument_id) const {
  if (config_.volatility.target_volatility > 0.0) {
    const double realized = realized_volatility(instrument_id);
    if (realized >=
        config_.volatility.target_volatility * config_.volatility.hard_reject_multiple) {
      return LegDecision{
          .verdict = RiskVerdict::kReject,
          .reason_code = ReasonCode::kVolatilityReduction,
          .reason = "realized volatility beyond hard-reject multiple of target at release"};
    }
  }
  return std::nullopt;
}

/// Control group 8-9: position limits against the projected position, and
/// per-order plus portfolio notional. Writes the portfolio-level overlay the
/// remaining group needs so the candidate is folded in exactly once.
std::optional<LegDecision> RiskEngine::check_position_and_notional(
    const OrderRequest& request, const EvaluationOverlay& overlay, std::int64_t effective_quantity,
    EvaluationOverlay& portfolio_overlay_out, std::int64_t& portfolio_notional_out) const {
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
      return LegDecision{.verdict = RiskVerdict::kReject,
                         .reason_code = ReasonCode::kMaxPositionLong,
                         .reason = "projected long position exceeds max_long_units"};
    }
    if (-projected > limit->second.max_short_units) {
      return LegDecision{.verdict = RiskVerdict::kReject,
                         .reason_code = ReasonCode::kMaxPositionShort,
                         .reason = "projected short position exceeds max_short_units"};
    }
  }

  // 9. Notional: per-order, then portfolio.
  bool unsupported_currency = false;
  const std::int64_t order_notional = notional_units_base_currency(
      request.instrument_id, effective_quantity, request.price_units, unsupported_currency);
  if (unsupported_currency) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kUnsupportedCurrency,
                       .reason = "instrument currency has no configured FX rate to base_currency"};
  }
  if (config_.max_order_notional_units > 0 && order_notional > config_.max_order_notional_units) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kMaxOrderNotional,
                       .reason = "order notional exceeds max_order_notional_units"};
  }
  EvaluationOverlay portfolio_overlay = overlay;
  portfolio_overlay.extra_reserved_by_instrument[request.instrument_id] += signed_candidate;
  const std::int64_t portfolio_notional = gross_portfolio_notional_units(portfolio_overlay);
  if (config_.max_portfolio_notional_units > 0 &&
      portfolio_notional > config_.max_portfolio_notional_units) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kMaxPortfolioNotional,
                       .reason = "portfolio notional exceeds max_portfolio_notional_units"};
  }
  portfolio_overlay_out = portfolio_overlay;
  portfolio_notional_out = portfolio_notional;
  return std::nullopt;
}

/// Control group 10-12: market/sector exposure, concentration and
/// correlated-group exposure, and margin plus leverage.
/// Control group 10: market and sector grouped exposure.
/// One grouping dimension (market or sector) of control group 10. The two
/// dimensions are structurally identical -- same "sum every configured
/// instrument sharing this key, compare to the configured limit" shape --
/// so they share one implementation rather than two copies that could drift.
std::optional<LegDecision> RiskEngine::check_one_exposure_group(
    const std::string& group_key, const std::unordered_map<std::string, std::int64_t>& limits,
    const std::function<const std::string&(const InstrumentInfo&)>& key_of,
    const EvaluationOverlay& portfolio_overlay, ReasonCode reason_code,
    const std::string& reason) const {
  if (group_key.empty()) {
    return std::nullopt;
  }
  const auto limit = limits.find(group_key);
  if (limit == limits.end()) {
    return std::nullopt;
  }
  // The group's total walks every configured instrument sharing this key, so
  // a market/sector spanning several instruments is summed, not approximated
  // by the candidate instrument alone.
  std::vector<std::uint32_t> members;
  for (const auto& [other_id, other_info] : config_.instruments) {
    if (key_of(other_info) == group_key) {
      members.push_back(other_id);
    }
  }
  if (group_gross_notional_units(members, portfolio_overlay) > limit->second) {
    return LegDecision{
        .verdict = RiskVerdict::kReject, .reason_code = reason_code, .reason = reason};
  }
  return std::nullopt;
}

std::optional<LegDecision> RiskEngine::check_group_exposure(
    const OrderRequest& request, const EvaluationOverlay& portfolio_overlay) const {
  // 10. Market/sector exposure.
  const auto info = config_.instruments.find(request.instrument_id);
  if (info == config_.instruments.end()) {
    return std::nullopt;
  }
  if (auto rejected = check_one_exposure_group(
          info->second.market, config_.market_exposure_limit_units,
          [](const InstrumentInfo& i) -> const std::string& { return i.market; }, portfolio_overlay,
          ReasonCode::kMarketExposure, "market exposure exceeds configured limit")) {
    return rejected;
  }
  return check_one_exposure_group(
      info->second.sector, config_.sector_exposure_limit_units,
      [](const InstrumentInfo& i) -> const std::string& { return i.sector; }, portfolio_overlay,
      ReasonCode::kSectorExposure, "sector exposure exceeds configured limit");
}

/// Control group 11: single-instrument concentration and configuration-supplied
/// correlated-group exposure (never an online-estimated correlation, ADR-0028).
///
/// M5 closure repair: the single-instrument numerator used to be computed
/// directly from `state_.position_units + state_.reserved_units +
/// signed_candidate` -- a hand-rolled shadow of the exact projected-exposure
/// computation `group_gross_notional_units` already performs for market/
/// sector/correlated-group exposure below. That duplication was the defect:
/// `portfolio_overlay` already carries the candidate's own contribution
/// counted exactly once (added by `check_position_and_notional`, and with
/// the leg's own committed reservation excluded first when called from
/// `revalidate_at_seam`), so re-adding `signed_candidate` on top of
/// `state_.reserved_units` double-counted it at the seam (where
/// `reserved_units` already includes this leg's own reservation) and,
/// during ordinary multi-leg preflight, ignored a same-instrument SIBLING
/// leg's contribution entirely (`overlay`/`portfolio_overlay`, not
/// `signed_candidate`, is what carries a sibling leg's exposure). Routing
/// through `group_gross_notional_units({request.instrument_id},
/// portfolio_overlay)` -- the identical function and identical overlay the
/// correlated-group branch two lines below already uses correctly -- fixes
/// both by construction: one shared projected-exposure model for every
/// cumulative control, never a second one for concentration alone.
std::optional<LegDecision> RiskEngine::check_concentration(
    const OrderRequest& request, const EvaluationOverlay& portfolio_overlay,
    std::int64_t portfolio_notional) const {
  // 11. Concentration and correlated-group exposure.
  if (config_.concentration.max_concentration_share < 1.0 && portfolio_notional > 0) {
    const std::int64_t instrument_notional =
        group_gross_notional_units({request.instrument_id}, portfolio_overlay);
    if (static_cast<double>(instrument_notional) / static_cast<double>(portfolio_notional) >
        config_.concentration.max_concentration_share) {
      return LegDecision{.verdict = RiskVerdict::kReject,
                         .reason_code = ReasonCode::kConcentration,
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
      return LegDecision{
          .verdict = RiskVerdict::kReject,
          .reason_code = ReasonCode::kCorrelatedExposure,
          .reason = "correlated group exposure exceeds configured limit: " + group_id};
    }
  }

  return std::nullopt;
}

/// Control group 12: simplified Model A margin and the leverage cap.
std::optional<LegDecision> RiskEngine::check_margin_and_leverage(
    const EvaluationOverlay& portfolio_overlay, std::int64_t portfolio_notional) const {
  // 12. Margin and leverage, using equity fed via on_equity_update.
  std::unordered_map<std::uint32_t, std::int64_t> exposure_for_margin = state_.all_positions();
  for (const auto& [instrument_id, reserved] : state_.all_reservations()) {
    exposure_for_margin[instrument_id] += reserved;
  }
  for (const auto& [instrument_id, extra] : portfolio_overlay.extra_reserved_by_instrument) {
    exposure_for_margin[instrument_id] += extra;
  }
  const std::int64_t required_margin =
      total_required_margin_units(config_.margin, exposure_for_margin);
  if (!config_.margin.margin_per_contract_units.empty() &&
      required_margin > state_.equity_units()) {
    return LegDecision{.verdict = RiskVerdict::kReject,
                       .reason_code = ReasonCode::kInsufficientMargin,
                       .reason = "required margin exceeds available equity"};
  }
  if (config_.max_leverage > 0.0) {
    if (state_.equity_units() <= 0 && portfolio_notional > 0) {
      return LegDecision{.verdict = RiskVerdict::kReject,
                         .reason_code = ReasonCode::kMaxLeverage,
                         .reason = "non-positive equity cannot support any leverage"};
    }
    if (state_.equity_units() > 0 &&
        static_cast<double>(portfolio_notional) / static_cast<double>(state_.equity_units()) >
            config_.max_leverage) {
      return LegDecision{.verdict = RiskVerdict::kReject,
                         .reason_code = ReasonCode::kMaxLeverage,
                         .reason = "gross notional / equity exceeds max_leverage"};
    }
  }
  return std::nullopt;
}

std::optional<LegDecision> RiskEngine::check_cumulative_controls(
    const OrderRequest& request, std::int64_t effective_quantity,
    const EvaluationOverlay& context_overlay) const {
  EvaluationOverlay portfolio_overlay;
  std::int64_t portfolio_notional = 0;
  if (auto rejected = check_position_and_notional(request, context_overlay, effective_quantity,
                                                  portfolio_overlay, portfolio_notional)) {
    return rejected;
  }
  if (auto rejected = check_group_exposure(request, portfolio_overlay)) {
    return rejected;
  }
  if (auto rejected = check_concentration(request, portfolio_overlay, portfolio_notional)) {
    return rejected;
  }
  if (auto rejected = check_margin_and_leverage(portfolio_overlay, portfolio_notional)) {
    return rejected;
  }
  return std::nullopt;
}

LegDecision RiskEngine::evaluate_leg(const OrderRequest& request,
                                     const EvaluationOverlay& overlay) const {
  // The control groups run in a fixed, documented order (ADR-0028): quantity
  // validity first (a malformed request is not legitimate input to ANY
  // later control, M5 closure repair R1), then halts and connectivity
  // (nothing else matters once trading is stopped), then market state, then
  // admission, then sizing, then the exposure-based limits that depend on
  // the approved size.
  if (auto rejected = check_quantity_validity(request)) {
    return *rejected;
  }
  if (auto rejected = check_halts_and_connectivity(request)) {
    return *rejected;
  }
  const MarketQuote* quote = nullptr;
  if (auto rejected = check_market_state(request, quote)) {
    return *rejected;
  }
  if (auto rejected = check_request_admission(request, overlay)) {
    return *rejected;
  }

  std::int64_t effective_quantity = request.quantity_units;
  bool resized = false;
  if (auto rejected = resolve_effective_quantity(request, effective_quantity, resized)) {
    return *rejected;
  }
  if (auto rejected = check_approved_quantity_postcondition(effective_quantity)) {
    return *rejected;
  }

  if (auto rejected = check_cumulative_controls(request, effective_quantity, overlay)) {
    return *rejected;
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

namespace {

ProposalDecisionResult all_legs_rejected(std::size_t leg_count, const LegDecision& rejection) {
  std::vector<LegDecision> all_rejected(leg_count, LegDecision{.verdict = RiskVerdict::kReject,
                                                               .reason_code = rejection.reason_code,
                                                               .reason = rejection.reason});
  return ProposalDecisionResult{.verdict = RiskVerdict::kReject,
                                .reason_code = rejection.reason_code,
                                .reason = rejection.reason,
                                .legs = std::move(all_rejected)};
}

}  // namespace

ProposalDecisionResult RiskEngine::evaluate_proposal(const std::string& strategy_id,
                                                     const std::string& proposal_id,
                                                     const std::vector<OrderRequest>& legs) const {
  // Phase A: per-leg non-cumulative admission (halts, connectivity, market
  // state, idempotency/rate-limit, order-quantity-cap/volatility sizing) --
  // none of these depend on a sibling leg's exposure, so evaluating them in
  // leg order is honest, not a prefix approximation. `admission_overlay`
  // stays a genuine running COUNT (rate limiting is a message-sequence
  // control, not a point-in-time exposure snapshot: the i-th leg really is
  // the i-th order in this burst). Alongside, build ONE final combined
  // `EvaluationOverlay` from every leg's own resolved quantity -- the
  // complete proposed portfolio, never a leg-by-leg accumulation that
  // omits legs not yet visited (the defect an independent review found: an
  // earlier leg could not see a LATER leg's exposure REDUCTION, so its own
  // cumulative check ran against a denominator that would shrink once the
  // later leg was accounted for, approving a proposal whose true combined
  // effect was unsafe).
  std::vector<OrderRequest> normalized_legs;
  normalized_legs.reserve(legs.size());
  std::vector<std::int64_t> effective_quantities;
  effective_quantities.reserve(legs.size());
  std::vector<bool> resized_flags;
  resized_flags.reserve(legs.size());

  EvaluationOverlay admission_overlay;
  EvaluationOverlay final_overlay;

  for (const OrderRequest& raw_leg : legs) {
    OrderRequest leg = raw_leg;
    leg.strategy_id = strategy_id;
    leg.proposal_id = proposal_id;

    // M5 closure repair, R1: an invalid quantity on ANY leg rejects the
    // WHOLE proposal atomically, exactly like any other Phase A admission
    // failure -- never a partial commit where a malformed sibling is
    // silently dropped and the rest proceed.
    if (auto rejected = check_quantity_validity(leg)) {
      return all_legs_rejected(legs.size(), *rejected);
    }
    if (auto rejected = check_halts_and_connectivity(leg)) {
      return all_legs_rejected(legs.size(), *rejected);
    }
    const MarketQuote* quote = nullptr;
    if (auto rejected = check_market_state(leg, quote)) {
      return all_legs_rejected(legs.size(), *rejected);
    }
    if (auto rejected = check_request_admission(leg, admission_overlay)) {
      return all_legs_rejected(legs.size(), *rejected);
    }
    admission_overlay.extra_orders_in_window += 1;

    std::int64_t effective_quantity = leg.quantity_units;
    bool resized = false;
    if (auto rejected = resolve_effective_quantity(leg, effective_quantity, resized)) {
      return all_legs_rejected(legs.size(), *rejected);
    }
    if (auto rejected = check_approved_quantity_postcondition(effective_quantity)) {
      return all_legs_rejected(legs.size(), *rejected);
    }

    const std::int64_t signed_quantity =
        leg.side == Side::kBuy ? effective_quantity : -effective_quantity;
    final_overlay.extra_reserved_by_instrument[leg.instrument_id] += signed_quantity;

    normalized_legs.push_back(std::move(leg));
    effective_quantities.push_back(effective_quantity);
    resized_flags.push_back(resized);
  }

  // Phase B: cumulative controls, each leg judged against the SAME final
  // combined overlay -- with only ITS OWN contribution excluded first, so
  // re-adding it as the candidate (inside check_cumulative_controls) counts
  // it exactly once. Identical technique to `revalidate_at_seam`'s
  // own-reservation exclusion; one shared model, not two.
  std::vector<LegDecision> decisions;
  decisions.reserve(normalized_legs.size());
  for (std::size_t index = 0; index < normalized_legs.size(); ++index) {
    const OrderRequest& leg = normalized_legs[index];
    const std::int64_t effective_quantity = effective_quantities[index];
    const std::int64_t signed_quantity =
        leg.side == Side::kBuy ? effective_quantity : -effective_quantity;

    EvaluationOverlay context_overlay = final_overlay;
    context_overlay.extra_reserved_by_instrument[leg.instrument_id] -= signed_quantity;

    if (auto rejected = check_cumulative_controls(leg, effective_quantity, context_overlay)) {
      return all_legs_rejected(legs.size(), *rejected);
    }
    decisions.push_back(LegDecision{
        .verdict = resized_flags[index] ? RiskVerdict::kResize : RiskVerdict::kApprove,
        .approved_quantity_units = effective_quantity,
        .reason_code = ReasonCode::kNone,
        .reason = "",
    });
  }

  const bool any_resize = std::ranges::any_of(resized_flags, [](bool r) { return r; });
  return ProposalDecisionResult{
      .verdict = any_resize ? RiskVerdict::kResize : RiskVerdict::kApprove,
      .reason_code = ReasonCode::kNone,
      .reason = "",
      .legs = std::move(decisions),
  };
}

const ProposalRiskDecision& RiskEngine::commit_proposal_decision(
    const std::string& strategy_id, const std::string& proposal_id,
    const std::vector<OrderRequest>& legs, common::Nanos decided_at_nanos) {
  // AEGIS-137 replay guard: proposal_id already has its ONE terminal
  // decision. Return it unchanged -- never re-decide, never re-arm, never
  // re-reserve, never append a second ProposalRiskDecision. A prior version
  // relied on the per-leg dedupe check inside evaluate_leg to reject a
  // replay, but that still fell through to an unconditional record_proposal
  // call below, appending a second record for the SAME proposal_id (the
  // AEGIS-137 defect an independent review found this exact test path
  // never asserted against).
  if (const ProposalRiskDecision* existing = audit_log_.find_proposal_decision(proposal_id)) {
    return *existing;
  }

  const ProposalDecisionResult result = evaluate_proposal(strategy_id, proposal_id, legs);

  if (result.verdict != RiskVerdict::kReject) {
    // Commit side effects only now -- rate-limit tokens, dedupe keys,
    // pending-leg arming AND exposure reservation all happen exactly once,
    // atomically, after the pure preflight above has already proven the
    // WHOLE proposal is admissible. Reserving here (not at decide_order) is
    // the fix for AEGIS-121..124/129: every cumulative control reads
    // state_.reserved_units/all_reservations, so a second proposal
    // committed before this one's orders reach the seam now correctly sees
    // this exposure already claimed.
    for (std::size_t index = 0; index < legs.size(); ++index) {
      const OrderRequest& leg = legs[index];
      const LegDecision& decision = result.legs[index];
      state_.mark_seen(dedupe_key(strategy_id, proposal_id, leg.leg_index));
      state_.record_order_event(leg.request_time_nanos);
      const PendingLegKey key{
          .strategy_id = strategy_id, .proposal_id = proposal_id, .leg_index = leg.leg_index};
      state_.reserve_leg(key, leg.instrument_id, leg.side, decision.approved_quantity_units,
                         strategy_id);
      // Canonical attribution originates HERE and only here (M5 closure
      // repair, N5): this is the one place `committed` becomes true, and
      // every one of stage/authorize/abort refuses to touch a record whose
      // `committed` is still false, so none of them can ever race to
      // establish ownership before a real commit.
      ProposalReleaseRecord& release_record = proposal_release_by_id_[proposal_id];
      release_record.strategy_id = strategy_id;
      release_record.committed = true;
      pending_legs_[key] = PendingLeg{
          .strategy_id = strategy_id,
          .proposal_id = proposal_id,
          .leg_index = leg.leg_index,
          .instrument_id = leg.instrument_id,
          .side = leg.side,
          .price_units = leg.price_units,
          .requested_quantity_units = leg.quantity_units,
          .approved_quantity_units = decision.approved_quantity_units,
          .verdict = decision.verdict,
          .reason_code = decision.reason_code,
          .reason = decision.reason,
      };
    }
  }

  return audit_log_.record_proposal(strategy_id, proposal_id, result.verdict, result.reason_code,
                                    result.reason, result.legs, decided_at_nanos);
}

void RiskEngine::stage_proposal_release(const std::string& strategy_id,
                                        const std::string& proposal_id,
                                        const std::vector<StagedOrderIdentity>& staged) {
  // M5 closure repair, N5: a proposal_id commit_proposal_decision has not
  // genuinely committed yet is UNKNOWN. Look up with find(), never
  // operator[] -- an unknown proposal gets no map entry at all, so a
  // pre-commit staging call can neither establish canonical ownership nor
  // leave any state a later legitimate commit would have to reckon with.
  const auto found = proposal_release_by_id_.find(proposal_id);
  if (found == proposal_release_by_id_.end() || !found->second.committed) {
    return;
  }
  ProposalReleaseRecord& record = found->second;

  // M5 closure repair, NB-2: for a proposal already known to be genuinely
  // committed, identity is verified BEFORE any mutation -- a wrong-strategy
  // staging call is UNAUTHORIZED API MISUSE, not evidence that the named
  // proposal itself is risky or malformed, and it must not mutate that
  // proposal in any way. An earlier version of this check latched an
  // `attribution_mismatch` flag and moved the record to `kStaging` on a
  // mismatch -- silently sabotaging the canonical owner's OWN future
  // `authorize_proposal_release` call, which discovered the flag and
  // rejected the WHOLE proposal (releasing every reservation, erasing
  // every armed leg, and misattributing the rejection to the victim's own
  // `strategy_id` in the audit trail, since canonical attribution itself
  // was never actually changed -- the victim's own rejection record wrongly
  // implied the victim had caused an identity mismatch it never caused).
  // Fixed to match `authorize_proposal_release` (N4) and
  // `abort_proposal_release` (N6): a mismatch returns immediately, before
  // touching `record.state`, `staged_by_leg_index` or anything else.
  if (record.strategy_id != strategy_id) {
    return;
  }

  if (record.state != ProposalReleaseState::kCommitted &&
      record.state != ProposalReleaseState::kStaging) {
    return;  // Already authorized, rejected, aborted or completed: staging is over.
  }
  record.state = ProposalReleaseState::kStaging;
  for (const StagedOrderIdentity& identity : staged) {
    record.staged_by_leg_index[identity.leg_index] = identity;
    // Bind the id now so decide_order can resolve it later WITHOUT ever
    // searching by economics. Binding is not authorization -- an order
    // whose proposal never reached kAuthorizedForRelease is rejected.
    order_identity_by_client_order_id_[identity.client_order_id] = PendingLegKey{
        .strategy_id = strategy_id, .proposal_id = proposal_id, .leg_index = identity.leg_index};
  }
}

std::optional<LegDecision> RiskEngine::revalidate_at_seam(const PendingLeg& pending,
                                                          common::Nanos now_nanos) const {
  const OrderRequest as_request{.strategy_id = pending.strategy_id,
                                .proposal_id = pending.proposal_id,
                                .leg_index = pending.leg_index,
                                .instrument_id = pending.instrument_id,
                                .side = pending.side,
                                .price_units = pending.price_units,
                                .quantity_units = pending.approved_quantity_units,
                                .request_time_nanos = now_nanos};

  // Halts/connectivity and market staleness/collar: these can change at any
  // moment between commit and seam arrival, and the seam is the LAST point
  // before adapter_->submit(), so this is where a late-breaking change must
  // actually bite. Deliberately NOT re-run here: idempotency (this leg's own
  // dedupe key was already marked seen at commit -- re-checking would always
  // reject) and the order-count rate limit (this leg's own event was already
  // recorded at commit -- re-checking double-counts a message that already
  // happened once). Neither omission reopens a safety gap: idempotency's
  // purpose (reject a REPLAYED submission) is already served by the
  // proposal-level replay guard in commit_proposal_decision, and the rate
  // limit's purpose (throttle a BURST of NEW admissions) was already served
  // when this leg was admitted at commit time.
  if (auto rejected = check_halts_and_connectivity(as_request)) {
    return rejected;
  }
  const MarketQuote* quote = nullptr;
  if (auto rejected = check_market_state(as_request, quote)) {
    return rejected;
  }

  // Volatility HARD-REJECT only (M5 closure repair, R6): a safety gate, not
  // a sizing decision, so unlike the resize branch it is re-checked fresh
  // here -- see check_volatility_hard_reject's own doc for why sizing stays
  // frozen while this does not.
  if (auto rejected = check_volatility_hard_reject(pending.instrument_id)) {
    return rejected;
  }

  // Cumulative exposure/margin/leverage, against CURRENT state -- but with
  // this leg's own already-reserved contribution subtracted out of the
  // overlay before being added back in as the candidate, so it is counted
  // exactly once (its own existing reservation), never twice (existing
  // reservation PLUS itself as a "new" candidate) and never zero times.
  const std::int64_t own_signed_reserved = pending.side == Side::kBuy
                                               ? pending.approved_quantity_units
                                               : -pending.approved_quantity_units;
  EvaluationOverlay overlay;
  overlay.extra_reserved_by_instrument[pending.instrument_id] -= own_signed_reserved;

  return check_cumulative_controls(as_request, pending.approved_quantity_units, overlay);
}

ProposalReleaseDecision RiskEngine::reject_proposal_release(const std::string& proposal_id,
                                                            ProposalReleaseRecord& record,
                                                            ReasonCode reason_code,
                                                            std::string reason,
                                                            common::Nanos now_nanos) {
  // One rejection path for every failure mode, so "the proposal was
  // rejected at release" always means exactly the same thing: nothing of
  // this proposal is armed, reserved or resolvable any more.
  for (auto it = pending_legs_.begin(); it != pending_legs_.end();) {
    if (it->first.proposal_id == proposal_id) {
      state_.release_leg_reservation(it->first);
      it = pending_legs_.erase(it);
    } else {
      ++it;
    }
  }
  record.state = ProposalReleaseState::kRejectedAtRelease;
  record.reason_code = reason_code;
  record.reason = std::move(reason);
  record.outstanding_authorized_legs = 0;
  audit_log_.record_proposal_release(record.strategy_id, proposal_id, /*authorized=*/false,
                                     reason_code, record.reason, now_nanos);
  return ProposalReleaseDecision{
      .state = record.state, .reason_code = record.reason_code, .reason = record.reason};
}

ProposalReleaseDecision RiskEngine::authorize_proposal_release(const std::string& strategy_id,
                                                               const std::string& proposal_id,
                                                               common::Nanos now_nanos) {
  // M5 closure repair, N5: an unknown/uncommitted proposal_id is looked up
  // with find(), never operator[] -- nothing is persisted for it, so a
  // pre-commit authorize call can neither establish ownership nor poison a
  // later legitimate commit of this id.
  const auto found = proposal_release_by_id_.find(proposal_id);
  if (found == proposal_release_by_id_.end() || !found->second.committed) {
    return ProposalReleaseDecision{.state = ProposalReleaseState::kRejectedAtRelease,
                                   .reason_code = ReasonCode::kUnexpectedOrder,
                                   .reason = "no committed proposal to authorize for release"};
  }
  ProposalReleaseRecord& record = found->second;

  // M5 closure repair, N4 (corrected): for a proposal already known to be
  // genuinely committed (the unknown/uncommitted case already returned
  // above), identity is verified BEFORE anything about this EXISTING
  // proposal's lifecycle state is inspected, mutated or disclosed. A
  // WRONG-STRATEGY AUTHORIZE CALL IS AN UNAUTHORIZED QUERY -- it is NOT a
  // risk rejection of the proposal it names, and it must not change the
  // proposal's state. Previously this check ran AFTER the terminal-state
  // lookup below and, on mismatch, called reject_proposal_release -- so a
  // caller with no relationship to this proposal could permanently destroy
  // it and reclaim its reserved risk budget, and a wrong-strategy query of
  // an already-terminal proposal received that proposal's REAL stored
  // decision. Neither reads nor mutates `record` beyond this one
  // comparison: no state change, no reservation release, no pending-leg
  // erasure, no audit event. The denial is fully generic and identical no
  // matter what this EXISTING proposal's real lifecycle state is (staged,
  // authorized, rejected, aborted or completed) -- it never reveals that
  // real stored decision. NOT CLAIMED (M5 closure repair, NB-1): this does
  // NOT make an unknown proposal_id indistinguishable from a wrong-strategy
  // one -- the branch above already returned a different reason code
  // (kUnexpectedOrder) for "never committed", so a caller can tell
  // "nonexistent" apart from "exists, wrong strategy" by reason code alone.
  // AEGIS claims no proposal-id confidentiality and no OS/process security
  // isolation; the guarantee is narrower: a wrong-strategy caller can never
  // mutate another strategy's proposal, nor read its real decision.
  if (record.strategy_id != strategy_id) {
    return ProposalReleaseDecision{
        .state = ProposalReleaseState::kRejectedAtRelease,
        .reason_code = ReasonCode::kIdentityMismatch,
        .reason =
            "authorize_proposal_release called with a strategy_id that disagrees with "
            "this proposal's canonical strategy_id"};
  }

  // Terminal and idempotent (M5 closure repair, N1): once the release
  // lifecycle reaches any of these four states, a later call from the
  // OWNING strategy (identity already verified above) never mutates it or
  // records a new audit event -- kAborted was the state missing here
  // before this repair, which let a call AFTER a deliberate
  // abort_proposal_release fall through below and overwrite the abort with
  // a spurious kUnexpectedOrder rejection. See authorize_proposal_release's
  // own header doc for why "at most one" is tracked per TRANSITION KIND
  // (authorize / reject / abort), not as a single release-record count.
  if (record.state == ProposalReleaseState::kAuthorizedForRelease ||
      record.state == ProposalReleaseState::kRejectedAtRelease ||
      record.state == ProposalReleaseState::kCompleted ||
      record.state == ProposalReleaseState::kAborted) {
    return ProposalReleaseDecision{
        .state = record.state, .reason_code = record.reason_code, .reason = record.reason};
  }

  // Every leg this proposal actually committed.
  std::vector<PendingLegKey> leg_keys;
  for (const auto& [key, leg] : pending_legs_) {
    if (key.proposal_id == proposal_id) {
      leg_keys.push_back(key);
    }
  }
  if (leg_keys.empty()) {
    return reject_proposal_release(proposal_id, record, ReasonCode::kUnexpectedOrder,
                                   "no committed legs to authorize for release", now_nanos);
  }

  // 1. Completeness: every committed leg needs a staged constituent, and no
  // staged constituent may name a leg this proposal never committed.
  if (record.staged_by_leg_index.size() != leg_keys.size()) {
    return reject_proposal_release(proposal_id, record, ReasonCode::kIncompleteProposalStaging,
                                   "staged constituent orders do not cover every committed leg",
                                   now_nanos);
  }
  for (const PendingLegKey& key : leg_keys) {
    if (!record.staged_by_leg_index.contains(key.leg_index)) {
      return reject_proposal_release(proposal_id, record, ReasonCode::kIncompleteProposalStaging,
                                     "a committed leg has no staged constituent order", now_nanos);
    }
  }

  // 2. Identity/economics: validated for EVERY constituent before ANY is
  // released, so a mismatch on one can never strand a correct sibling.
  for (const PendingLegKey& key : leg_keys) {
    const PendingLeg& leg = pending_legs_.at(key);
    const StagedOrderIdentity& staged = record.staged_by_leg_index.at(key.leg_index);
    if (staged.instrument_id != leg.instrument_id || staged.side != leg.side ||
        staged.quantity_units != leg.requested_quantity_units) {
      return reject_proposal_release(proposal_id, record, ReasonCode::kIdentityMismatch,
                                     "a staged constituent's economics disagree with its "
                                     "committed leg",
                                     now_nanos);
    }
  }

  // 3. Fresh whole-proposal safety, at a moment when rejecting is still free.
  for (const PendingLegKey& key : leg_keys) {
    const PendingLeg& leg = pending_legs_.at(key);
    if (auto rejected = revalidate_at_seam(leg, now_nanos)) {
      return reject_proposal_release(proposal_id, record, rejected->reason_code, rejected->reason,
                                     now_nanos);
    }
  }

  record.state = ProposalReleaseState::kAuthorizedForRelease;
  record.reason_code = ReasonCode::kNone;
  record.reason.clear();
  record.outstanding_authorized_legs = leg_keys.size();
  audit_log_.record_proposal_release(record.strategy_id, proposal_id, /*authorized=*/true,
                                     ReasonCode::kNone, "", now_nanos);
  return ProposalReleaseDecision{
      .state = record.state, .reason_code = ReasonCode::kNone, .reason = ""};
}

ProposalReleaseState RiskEngine::proposal_release_state(const std::string& proposal_id) const {
  const auto found = proposal_release_by_id_.find(proposal_id);
  return found == proposal_release_by_id_.end() ? ProposalReleaseState::kCommitted
                                                : found->second.state;
}

ProposalReleaseDecision RiskEngine::abort_proposal_release(const std::string& strategy_id,
                                                           const std::string& proposal_id,
                                                           std::string reason,
                                                           common::Nanos now_nanos) {
  // M5 closure repair, N6: an unknown/uncommitted proposal_id is looked up
  // with find(), never operator[] -- nothing is persisted for it, so a
  // pre-emptive abort can never poison a later legitimate commit of this id.
  const auto found = proposal_release_by_id_.find(proposal_id);
  if (found == proposal_release_by_id_.end() || !found->second.committed) {
    return ProposalReleaseDecision{.state = ProposalReleaseState::kRejectedAtRelease,
                                   .reason_code = ReasonCode::kUnexpectedOrder,
                                   .reason = "no committed proposal to abort"};
  }
  ProposalReleaseRecord& record = found->second;

  // M5 closure repair, N6: only the proposal's own canonical strategy may
  // abort it -- a caller cannot abort a DIFFERENT strategy's proposal
  // merely by knowing its proposal_id. A wrong-strategy call is a pure
  // no-op: it returns the CURRENT (unaffected) decision, mutates nothing,
  // and is never audited as an attempt against this proposal.
  if (record.strategy_id != strategy_id) {
    return ProposalReleaseDecision{
        .state = record.state, .reason_code = record.reason_code, .reason = record.reason};
  }

  // Idempotent and terminal, matching authorize_proposal_release's own
  // contract: a proposal that is already kAborted, kRejectedAtRelease or
  // kCompleted returns its existing decision unchanged and records no
  // second audit entry. Aborting a proposal that was never authorized
  // (kCommitted/kStaging) is also honoured -- it simply releases whatever
  // reservations exist yet.
  if (record.state == ProposalReleaseState::kAborted ||
      record.state == ProposalReleaseState::kRejectedAtRelease ||
      record.state == ProposalReleaseState::kCompleted) {
    return ProposalReleaseDecision{
        .state = record.state, .reason_code = record.reason_code, .reason = record.reason};
  }

  // Release only what is still unconsumed. A leg that already consumed its
  // authorization through decide_order is no longer in pending_legs_ (it
  // erases its own entry there and transitions to an order-keyed
  // reservation) -- this walk therefore never touches a live or terminal
  // order, exactly like reject_proposal_release's own walk.
  for (auto it = pending_legs_.begin(); it != pending_legs_.end();) {
    if (it->first.proposal_id == proposal_id) {
      state_.release_leg_reservation(it->first);
      it = pending_legs_.erase(it);
    } else {
      ++it;
    }
  }

  record.state = ProposalReleaseState::kAborted;
  record.reason_code = ReasonCode::kProposalAborted;
  record.reason = std::move(reason);
  record.outstanding_authorized_legs = 0;
  audit_log_.record_proposal_release(record.strategy_id, proposal_id, /*authorized=*/false,
                                     record.reason_code, record.reason, now_nanos);
  return ProposalReleaseDecision{
      .state = record.state, .reason_code = record.reason_code, .reason = record.reason};
}

OmsDecision RiskEngine::decide_order(std::uint32_t instrument_id, Side side,
                                     std::int64_t quantity_units, std::uint64_t client_order_id,
                                     common::Nanos now_nanos) {
  // decide_order is CONSUMPTION ONLY (ADR-0027 "Correction 3"). Every
  // proposal-level safety question was answered by
  // authorize_proposal_release, at a moment when no constituent of this
  // proposal had been released. Re-asking any of them here -- kill switch,
  // connectivity, staleness, or any cumulative control -- is exactly what
  // would let this leg contradict a sibling that already went out, which is
  // the naked-leg hazard the release epoch exists to prevent.
  const auto identity_found = order_identity_by_client_order_id_.find(client_order_id);
  if (identity_found == order_identity_by_client_order_id_.end()) {
    audit_log_.record_order("", 0, client_order_id, instrument_id, RiskVerdict::kReject,
                            quantity_units, 0, ReasonCode::kUnexpectedOrder,
                            "no staged proposal constituent for this order", now_nanos);
    return OmsDecision{.verdict = RiskVerdict::kReject,
                       .approved_quantity_units = 0,
                       .reason_code = ReasonCode::kUnexpectedOrder,
                       .reason = "no staged proposal constituent for this order"};
  }
  const PendingLegKey key = identity_found->second;

  // The proposal's ONE release decision governs this order absolutely.
  const auto release_found = proposal_release_by_id_.find(key.proposal_id);
  const ProposalReleaseState release_state = release_found == proposal_release_by_id_.end()
                                                 ? ProposalReleaseState::kCommitted
                                                 : release_found->second.state;
  if (release_state != ProposalReleaseState::kAuthorizedForRelease) {
    // Staged-but-unauthorized, rejected at release, or already completed --
    // none of which is permission. A rejected proposal reports the reason
    // its release authorization actually failed for, so the audit trail
    // says why rather than merely that.
    ReasonCode reason_code = ReasonCode::kProposalNotAuthorized;
    std::string reason = "proposal has no release authorization";
    if (release_state == ProposalReleaseState::kRejectedAtRelease ||
        release_state == ProposalReleaseState::kAborted) {
      reason_code = release_found->second.reason_code;
      reason = release_found->second.reason;
    }
    audit_log_.record_order(key.proposal_id, key.leg_index, client_order_id, instrument_id,
                            RiskVerdict::kReject, quantity_units, 0, reason_code, reason,
                            now_nanos);
    return OmsDecision{.verdict = RiskVerdict::kReject,
                       .approved_quantity_units = 0,
                       .reason_code = reason_code,
                       .reason = reason};
  }

  const auto found = pending_legs_.find(key);
  if (found == pending_legs_.end()) {
    // An authorized constituent consumed twice: the authorization is
    // per-leg single-use, so the second attempt is not executable.
    audit_log_.record_order(key.proposal_id, key.leg_index, client_order_id, instrument_id,
                            RiskVerdict::kReject, quantity_units, 0, ReasonCode::kUnexpectedOrder,
                            "this proposal constituent already consumed its authorization",
                            now_nanos);
    return OmsDecision{.verdict = RiskVerdict::kReject,
                       .approved_quantity_units = 0,
                       .reason_code = ReasonCode::kUnexpectedOrder,
                       .reason = "this proposal constituent already consumed its authorization"};
  }
  const PendingLeg pending = found->second;

  // Defensive backstop only: authorize_proposal_release already verified
  // every constituent's economics against its committed leg, so reaching
  // this branch means the OMS submitted something other than what was
  // staged. Blocker B is closed structurally at the epoch above, not here.
  if (pending.instrument_id != instrument_id || pending.side != side ||
      pending.requested_quantity_units != quantity_units) {
    audit_log_.record_order(pending.proposal_id, pending.leg_index, client_order_id, instrument_id,
                            RiskVerdict::kReject, quantity_units, 0, ReasonCode::kIdentityMismatch,
                            "submitted order disagrees with the staged, authorized constituent",
                            now_nanos);
    return OmsDecision{
        .verdict = RiskVerdict::kReject,
        .approved_quantity_units = 0,
        .reason_code = ReasonCode::kIdentityMismatch,
        .reason = "submitted order disagrees with the staged, authorized constituent"};
  }

  // Consume: the reservation already exists (commit_proposal_decision); this
  // only re-keys it to client_order_id for the ordinary fill/release
  // lifecycle. Never a second reservation.
  pending_legs_.erase(key);
  order_identity_by_client_order_id_.erase(client_order_id);
  state_.transition_leg_reservation_to_order(key, client_order_id);
  if (release_found != proposal_release_by_id_.end() &&
      release_found->second.outstanding_authorized_legs > 0) {
    release_found->second.outstanding_authorized_legs -= 1;
    if (release_found->second.outstanding_authorized_legs == 0) {
      release_found->second.state = ProposalReleaseState::kCompleted;
    }
  }

  audit_log_.record_order(pending.proposal_id, pending.leg_index, client_order_id, instrument_id,
                          pending.verdict, quantity_units, pending.approved_quantity_units,
                          pending.reason_code, pending.reason, now_nanos);
  return OmsDecision{.verdict = pending.verdict,
                     .approved_quantity_units = pending.approved_quantity_units,
                     .reason_code = pending.reason_code,
                     .reason = pending.reason};
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
  // M5 closure repair, N3: a fill quantity is a positive magnitude, exactly
  // like a request/approved quantity (R1/N2) -- `side` alone carries
  // direction. A non-positive value is malformed input from whatever
  // upstream event source reported it, never a legitimate zero-size or
  // opposite-direction fill; applying it would silently poison confirmed
  // position (`RiskState::apply_fill`) and every cumulative control that
  // reads it. Ignored with no state mutation, exactly as a caller reporting
  // no fill at all would be.
  if (fill_quantity_units <= 0) {
    return;
  }
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
