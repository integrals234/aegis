#include "cpp/participant/app/participant_run.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cpp/common/clock.hpp"
#include "cpp/events/envelope.hpp"
#include "cpp/events/exchange_messages.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/participant/app/participant_snapshot.hpp"
#include "cpp/participant/app/risk_engine_gate.hpp"
#include "cpp/participant/book_builder/book_builder.hpp"
#include "cpp/participant/feed_handler/feed_handler.hpp"
#include "cpp/participant/oms/execution_adapter.hpp"
#include "cpp/participant/oms/order_lifecycle.hpp"
#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/oms/recorded_response_adapter.hpp"
#include "cpp/participant/oms/risk_gate.hpp"
#include "cpp/participant/portfolio/portfolio.hpp"
#include "cpp/participant/portfolio/portfolio_risk.hpp"
#include "cpp/participant/risk/risk_engine.hpp"
#include "cpp/participant/strategy/calendar_spread_strategy.hpp"
#include "cpp/statistics/rolling_moments.hpp"

namespace aegis::participant::app {
namespace {

using events::Envelope;
using events::MessageType;
using events::exchange::Side;
using events::exchange::TradeEvent;
using events::market_data::BookDeltaEvent;
using events::market_data::BookLevelEntry;
using events::market_data::BookSnapshotEvent;
using events::market_data::DeltaKind;

constexpr std::uint32_t kInstrumentId = 1001;

Envelope frame(MessageType type, std::uint64_t sequence, std::vector<std::byte> payload) {
  Envelope envelope;
  envelope.message_type = type;
  envelope.sequence = sequence;
  envelope.stream_id = 1;
  envelope.payload = std::move(payload);
  return envelope;
}

}  // namespace

// ---------------------------------------------------------------------------
// AEGIS-237; ADR-0024: fixture-driven scenario runner + process-boundary
// recovery. Kept separate from the builtin-scenario helpers above (which
// stay exactly as Checkpoint 1 left them) because this path has a genuinely
// different job: replaying a committed step sequence deterministically,
// optionally split across a snapshot boundary.
// ---------------------------------------------------------------------------
namespace {

using Json = nlohmann::json;
using events::exchange::OrderAcceptedEvent;
using events::exchange::OrderRejectedEvent;
using events::exchange::OrderTerminatedEvent;
using events::exchange::OrderType;
using events::exchange::RejectReason;
using events::exchange::TerminationReason;
using oms::OrderManager;
using oms::OrderState;
using oms::RecordedResponseAdapter;
using oms::RiskDecision;
using oms::RiskGate;
using oms::RiskVerdict;
using oms::TrackedOrder;
using portfolio::Portfolio;
using portfolio::Position;

/// Test/fixture double (ADR-0023): no production `RiskGate` implementation
/// ships before M5. Every fixture-driven order is approved as submitted --
/// the fixture's own steps are what decide whether it then fills, rests,
/// or is rejected by the (simulated) exchange.
class AlwaysApproveRiskGate final : public RiskGate {
 public:
  [[nodiscard]] RiskDecision decide(
      const events::exchange::NewOrderCommand& /*command*/) const override {
    return RiskDecision{
        .verdict = RiskVerdict::kApprove, .resized_quantity_units = 0, .reason = ""};
  }
};

[[nodiscard]] Side parse_fixture_side(const std::string& value) {
  return value == "SELL" ? Side::kSell : Side::kBuy;
}

[[nodiscard]] OrderType parse_fixture_order_type(const std::string& value) {
  return value == "MARKET" ? OrderType::kMarket : OrderType::kLimit;
}

[[nodiscard]] TerminationReason parse_termination_reason(const std::string& value) {
  if (value == "CANCELED") {
    return TerminationReason::kCanceled;
  }
  if (value == "RESIDUAL_CANCELED") {
    return TerminationReason::kResidualCanceled;
  }
  if (value == "REPLACED") {
    return TerminationReason::kReplaced;
  }
  return TerminationReason::kFilled;
}

[[nodiscard]] RejectReason parse_reject_reason(const std::string& value) {
  if (value == "UNKNOWN_ORDER_ID") {
    return RejectReason::kUnknownOrderId;
  }
  if (value == "DUPLICATE_CLIENT_ORDER_ID") {
    return RejectReason::kDuplicateClientOrderId;
  }
  return RejectReason::kPriceOutOfBand;
}

/// Reads every non-empty line of `path` as one JSON object -- the fixture's
/// step sequence, in committed order.
[[nodiscard]] std::vector<Json> read_fixture_steps(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("cannot open fixture " + path);
  }
  std::vector<Json> steps;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) {
      continue;
    }
    steps.push_back(Json::parse(line));
  }
  return steps;
}

[[nodiscard]] std::vector<std::byte> read_file_bytes(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  file.seekg(0);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  // A snapshot is opaque binary, not text (mirrors replay_main.cpp's
  // identical helper).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  file.read(reinterpret_cast<char*>(bytes.data()), size);
  return bytes;
}

void write_file_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path + " for writing");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* data = reinterpret_cast<const char*>(bytes.data());
  file.write(data, static_cast<std::streamsize>(bytes.size()));
}

/// Applies `trade`'s fill to `ledger` for whichever side is a tracked order
/// in `manager` -- a no-op for the side (or both sides) this manager is not
/// tracking, mirroring `test_participant_exchange_integration.cpp`'s
/// `deliver()` helper generalized to look up either side rather than one
/// hardcoded id.
///
/// AEGIS-116: the fee charged to the ledger is the one the fixture declares,
/// and the same `FeeSchedule` is given to `OrderManager` so its own accrual
/// agrees with what cash is actually charged. The M3 closure audit found two
/// independent fee ledgers here -- a fixture-supplied `fee_units` reaching
/// the portfolio while the OMS accrued its own at a default rate of zero --
/// which could disagree without anything noticing.
void apply_trade_to_portfolio(const TradeEvent& trade, const OrderManager& manager,
                              Portfolio& ledger, std::int64_t fee_units) {
  if (manager.find_by_exchange_order_id(trade.maker_order_id) != nullptr) {
    const Side maker_side = trade.taker_side == Side::kBuy ? Side::kSell : Side::kBuy;
    ledger.apply_fill(trade.instrument_id, maker_side, trade.price_units, trade.quantity_units,
                      fee_units);
  }
  if (manager.find_by_exchange_order_id(trade.taker_order_id) != nullptr) {
    ledger.apply_fill(trade.instrument_id, trade.taker_side, trade.price_units,
                      trade.quantity_units, fee_units);
  }
}

/// One canonical JSON line summarizing the entire current OMS + portfolio
/// state -- a deterministic function of the state alone, so two runs (or
/// two halves of one split run) can be compared byte for byte without
/// depending on any internal type.
[[nodiscard]] std::string describe_state(const OrderManager& manager, const Portfolio& ledger) {
  Json out;
  Json orders = Json::array();
  for (const TrackedOrder& tracked : manager.all_tracked_orders()) {
    orders.push_back(Json{
        {"client_order_id", tracked.client_order_id},
        {"exchange_order_id", tracked.exchange_order_id},
        {"instrument_id", tracked.instrument_id},
        {"participant_id", tracked.participant_id},
        {"side", static_cast<int>(tracked.side)},
        {"lifecycle_state", static_cast<int>(tracked.lifecycle.state())},
        {"price_units", tracked.price_units},
        {"original_quantity_units", tracked.original_quantity_units},
        {"cumulative_filled_units", tracked.cumulative_filled_units},
        {"remaining_units", tracked.remaining_units},
    });
  }
  out["orders"] = orders;
  out["next_client_order_id"] = manager.next_client_order_id();

  Json positions = Json::array();
  for (const auto& [instrument_id, position] : ledger.all_positions()) {
    positions.push_back(Json{
        {"instrument_id", instrument_id},
        {"quantity_units", position.quantity_units},
        {"average_price_units", position.average_price_units},
        {"realized_pnl_units", position.realized_pnl_units},
    });
  }
  out["positions"] = positions;
  out["cash_units"] = ledger.cash_units();

  return out.dump();
}

void apply_step(const Json& step, OrderManager& manager, Portfolio& ledger) {
  const auto kind = step.at("kind").get<std::string>();
  if (kind == "submit_new_order") {
    static_cast<void>(manager.submit_new_order(
        step.at("instrument_id").get<std::uint32_t>(),
        step.at("participant_id").get<std::uint64_t>(),
        parse_fixture_side(step.at("side").get<std::string>()),
        parse_fixture_order_type(step.at("order_type").get<std::string>()),
        step.at("price_units").get<std::int64_t>(), step.at("quantity_units").get<std::int64_t>()));
  } else if (kind == "cancel_order") {
    static_cast<void>(manager.cancel_order(step.at("client_order_id").get<std::uint64_t>()));
  } else if (kind == "order_accepted") {
    manager.handle_order_accepted(OrderAcceptedEvent{
        .order_id = step.at("order_id").get<std::uint64_t>(),
        .instrument_id = step.at("instrument_id").get<std::uint32_t>(),
        .participant_id = step.at("participant_id").get<std::uint64_t>(),
        .client_order_id = step.at("client_order_id").get<std::uint64_t>(),
        .side = parse_fixture_side(step.at("side").get<std::string>()),
        .order_type = parse_fixture_order_type(step.at("order_type").get<std::string>()),
        .price_units = step.at("price_units").get<std::int64_t>(),
        .quantity_units = step.at("quantity_units").get<std::int64_t>(),
    });
  } else if (kind == "order_rejected") {
    manager.handle_order_rejected(OrderRejectedEvent{
        .instrument_id = step.at("instrument_id").get<std::uint32_t>(),
        .participant_id = step.at("participant_id").get<std::uint64_t>(),
        .client_order_id = step.value("client_order_id", std::uint64_t{0}),
        .order_id = step.value("order_id", std::uint64_t{0}),
        .reason = parse_reject_reason(step.at("reason").get<std::string>()),
    });
  } else if (kind == "trade") {
    const TradeEvent trade{
        .instrument_id = step.at("instrument_id").get<std::uint32_t>(),
        .price_units = step.at("price_units").get<std::int64_t>(),
        .quantity_units = step.at("quantity_units").get<std::int64_t>(),
        .maker_order_id = step.at("maker_order_id").get<std::uint64_t>(),
        .taker_order_id = step.at("taker_order_id").get<std::uint64_t>(),
        .maker_participant_id = step.value("maker_participant_id", std::uint64_t{0}),
        .taker_participant_id = step.value("taker_participant_id", std::uint64_t{0}),
        .taker_side = parse_fixture_side(step.at("taker_side").get<std::string>()),
    };
    manager.handle_trade(trade);
    apply_trade_to_portfolio(trade, manager, ledger, step.value("fee_units", std::int64_t{0}));
  } else if (kind == "order_terminated") {
    manager.handle_order_terminated(OrderTerminatedEvent{
        .order_id = step.at("order_id").get<std::uint64_t>(),
        .reason = parse_termination_reason(step.at("reason").get<std::string>()),
        .cancelled_quantity_delta_units =
            step.value("cancelled_quantity_delta_units", std::int64_t{0}),
    });
  } else {
    throw std::runtime_error("unknown fixture step kind: " + kind);
  }
}

}  // namespace

RunSummary run_builtin_scenario() {
  feed::FeedHandler handler;
  book::BookBuilder book(kInstrumentId);
  stats::RollingMoments trade_prices(/*window=*/5);

  // 1. A full-depth snapshot: one resting bid, one resting ask.
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = kInstrumentId;
  snapshot.md_sequence = 1;
  snapshot.entries.push_back(BookLevelEntry{
      .side = Side::kBuy, .price_units = 10'000, .quantity_units = 50, .order_id = 1});
  snapshot.entries.push_back(BookLevelEntry{
      .side = Side::kSell, .price_units = 10'010, .quantity_units = 40, .order_id = 2});
  {
    const auto decoded =
        handler.decode(frame(MessageType::kBookSnapshot, 1, events::market_data::encode(snapshot)));
    if (decoded.snapshot.has_value()) {
      book.apply_snapshot(*decoded.snapshot);
    }
  }

  // 2. Two incremental deltas: a new bid, and a modify on the resting bid.
  BookDeltaEvent added;
  added.instrument_id = kInstrumentId;
  added.md_sequence = 2;
  added.kind = DeltaKind::kOrderAdded;
  added.order_id = 3;
  added.side = Side::kBuy;
  added.price_units = 9'990;
  added.quantity_units = 20;
  {
    const auto decoded =
        handler.decode(frame(MessageType::kBookDelta, 2, events::market_data::encode(added)));
    if (decoded.delta.has_value()) {
      book.apply_delta(*decoded.delta);
    }
  }

  BookDeltaEvent modified;
  modified.instrument_id = kInstrumentId;
  modified.md_sequence = 3;
  modified.kind = DeltaKind::kOrderModified;
  modified.order_id = 1;
  modified.side = Side::kBuy;
  modified.price_units = 10'000;
  modified.quantity_units = 30;
  {
    const auto decoded =
        handler.decode(frame(MessageType::kBookDelta, 3, events::market_data::encode(modified)));
    if (decoded.delta.has_value()) {
      book.apply_delta(*decoded.delta);
    }
  }

  // 3. Three trades, decoded and pushed into a generic rolling statistic.
  const std::array<std::int64_t, 3> trade_prices_units{10'005, 10'007, 10'004};
  std::uint32_t trade_count = 0;
  for (const std::int64_t price : trade_prices_units) {
    TradeEvent trade;
    trade.instrument_id = kInstrumentId;
    trade.price_units = price;
    trade.quantity_units = 10;
    trade.maker_order_id = 1;
    trade.taker_order_id = 99;
    trade.taker_side = Side::kBuy;
    const auto decoded = handler.decode(
        frame(MessageType::kTrade, 100 + trade_count, events::exchange::encode(trade)));
    if (decoded.trade.has_value()) {
      trade_prices.push(static_cast<double>(decoded.trade->price_units));
      ++trade_count;
    }
  }

  // 4. OMS: one order through its full happy-path lifecycle. Every step here
  // is a legal transition by construction (test_order_lifecycle.cpp proves
  // the table), so the [[nodiscard]] result is discarded deliberately.
  oms::OrderLifecycle lifecycle;
  static_cast<void>(lifecycle.transition(oms::OrderState::kRiskPending));
  static_cast<void>(lifecycle.transition(oms::OrderState::kSubmitted));
  static_cast<void>(lifecycle.transition(oms::OrderState::kAcknowledged));
  static_cast<void>(lifecycle.transition(oms::OrderState::kFilled));

  // 5. Portfolio: the fill the OMS lifecycle above represents.
  portfolio::Portfolio ledger;
  ledger.apply_fill(kInstrumentId, Side::kBuy, /*price_units=*/10'005, /*quantity_units=*/10,
                    /*fee_units=*/1);
  const portfolio::Position position = ledger.position(kInstrumentId);

  RunSummary summary;
  summary.instrument_id = kInstrumentId;
  if (const auto bid = book.best(Side::kBuy); bid.has_value()) {
    summary.best_bid_price_units = bid->price_units;
    summary.best_bid_quantity_units = bid->quantity_units;
  }
  if (const auto ask = book.best(Side::kSell); ask.has_value()) {
    summary.best_ask_price_units = ask->price_units;
    summary.best_ask_quantity_units = ask->quantity_units;
  }
  summary.last_md_sequence = book.last_md_sequence();
  summary.microprice = book.microprice();
  summary.trade_count = trade_count;
  summary.trade_price_rolling_mean = trade_prices.mean();
  summary.final_order_state = static_cast<std::uint8_t>(lifecycle.state());
  summary.position_quantity_units = position.quantity_units;
  summary.position_average_price_units = position.average_price_units;
  summary.realized_pnl_units = position.realized_pnl_units;
  summary.unrealized_pnl_units =
      ledger.unrealized_pnl_units(kInstrumentId, /*mark_price_units=*/10'010);
  summary.cash_units = ledger.cash_units();
  return summary;
}

FixtureRunResult run_participant_fixture(const std::string& fixture_path, std::uint64_t skip,
                                         std::optional<std::uint64_t> limit,
                                         const std::optional<std::string>& restore_from_path,
                                         const std::optional<std::string>& snapshot_out_path) {
  const auto steps = read_fixture_steps(fixture_path);

  AlwaysApproveRiskGate risk;
  RecordedResponseAdapter adapter({});
  // One fee authority: the scenario's own rate, shared by the OMS accrual
  // and the portfolio charge below (AEGIS-116).
  const oms::FeeSchedule fees{.fee_rate_ppm = 0};

  std::optional<OrderManager> manager_storage;
  std::optional<Portfolio> ledger_storage;
  if (restore_from_path.has_value()) {
    const auto read_result = read_participant_snapshot(read_file_bytes(*restore_from_path));
    if (!read_result.has_value()) {
      throw std::runtime_error("failed to restore participant snapshot: " +
                               std::string{describe(read_result.error())});
    }
    const ParticipantSnapshot& snapshot = read_result.value();
    manager_storage.emplace(adapter, risk, oms::to_tracked_orders(snapshot.oms),
                            snapshot.oms.next_client_order_id, fees);
    ledger_storage.emplace(portfolio::restore_portfolio(snapshot.portfolio));
  } else {
    manager_storage.emplace(adapter, risk, fees);
    ledger_storage.emplace();
  }
  OrderManager& manager = *manager_storage;
  Portfolio& ledger = *ledger_storage;

  const auto step_limit = limit.value_or(steps.size());
  FixtureRunResult result;
  std::uint64_t applied = 0;
  for (std::uint64_t i = skip; i < steps.size() && applied < step_limit; ++i, ++applied) {
    apply_step(steps[i], manager, ledger);
    result.lines.push_back(describe_state(manager, ledger));
  }

  if (snapshot_out_path.has_value()) {
    const ParticipantSnapshot snapshot = capture_participant_snapshot(manager, ledger);
    write_file_bytes(*snapshot_out_path, write_participant_snapshot(snapshot));
  }

  return result;
}

// ---------------------------------------------------------------------------
// AEGIS-076..081, AEGIS-004, AEGIS-080; ADR-0025: the first M4 strategy,
// wired here to the existing risk seam and OMS. See participant_run.hpp for
// the composition this exercises and its relationship to the real-M1-matching
// proof in tests/cpp/unit/test_calendar_spread_exchange_integration.cpp.
// ---------------------------------------------------------------------------
namespace {

using strategy::CalendarSpreadConfig;
using strategy::CalendarSpreadStrategy;
using strategy::StrategyLeg;
using strategy::StrategyProposal;

/// No transport, no exchange (AEGIS-119): `submit`/`cancel`/`modify` only
/// record that a call was made. The composition root below synthesizes the
/// resulting accept/trade/terminate sequence itself, deterministically, from
/// this run's own strategy-generated intent -- not from a committed script
/// (`RecordedResponseAdapter`) and not from real matching
/// (`TransportExecutionAdapter`, `tests/` only).
class ImmediateFillExecutionAdapter final : public oms::ExecutionAdapter {
 public:
  [[nodiscard]] bool submit(const events::exchange::NewOrderCommand& /*command*/) override {
    return true;
  }
  [[nodiscard]] bool cancel(const events::exchange::CancelOrderCommand& /*command*/) override {
    return true;
  }
  [[nodiscard]] bool modify(const events::exchange::ModifyOrderCommand& /*command*/) override {
    return true;
  }
};

struct MarketDataStep {
  std::string leg;  ///< "near" or "far".
  std::uint32_t instrument_id{0};
  std::uint64_t md_sequence{0};
  std::int64_t bid_price_units{0};
  std::int64_t bid_quantity_units{0};
  std::int64_t ask_price_units{0};
  std::int64_t ask_quantity_units{0};
};

[[nodiscard]] std::vector<MarketDataStep> read_market_data_steps(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("cannot open calendar-spread stream " + path);
  }
  std::vector<MarketDataStep> steps;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) {
      continue;
    }
    const Json record = Json::parse(line);
    MarketDataStep step;
    step.leg = record.at("leg").get<std::string>();
    step.instrument_id = record.at("instrument_id").get<std::uint32_t>();
    step.md_sequence = record.at("md_sequence").get<std::uint64_t>();
    step.bid_price_units = record.at("bid_price_units").get<std::int64_t>();
    step.bid_quantity_units = record.at("bid_quantity_units").get<std::int64_t>();
    step.ask_price_units = record.at("ask_price_units").get<std::int64_t>();
    step.ask_quantity_units = record.at("ask_quantity_units").get<std::int64_t>();
    steps.push_back(std::move(step));
  }
  return steps;
}

[[nodiscard]] BookSnapshotEvent make_two_sided_snapshot(const MarketDataStep& step) {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = step.instrument_id;
  snapshot.md_sequence = step.md_sequence;
  snapshot.entries.push_back(BookLevelEntry{.side = Side::kBuy,
                                            .price_units = step.bid_price_units,
                                            .quantity_units = step.bid_quantity_units,
                                            .order_id = 1});
  snapshot.entries.push_back(BookLevelEntry{.side = Side::kSell,
                                            .price_units = step.ask_price_units,
                                            .quantity_units = step.ask_quantity_units,
                                            .order_id = 2});
  return snapshot;
}

/// One leg's order through the mandatory risk seam (the real M5
/// `risk::RiskEngine`, via `RiskEngineGate` -- ADR-0027), a deterministic
/// immediate fill at the APPROVED quantity -- never the strategy's original
/// ask: a `kResize` verdict changes what `OrderManager` actually submits,
/// and `tracked->original_quantity_units` is what `OrderManager` itself
/// recorded after applying that resize -- and the portfolio + risk fill
/// feedback. Returns the assigned `client_order_id`, or 0 if the seam
/// rejected the order: the composition root only calls this after a
/// proposal-level approve/resize, so a per-leg reject here means the seam
/// disagreed with the already-committed proposal decision, which a
/// well-formed run never produces but this function still handles safely
/// rather than fabricate a fill.
std::uint64_t execute_leg(const StrategyLeg& leg, OrderManager& manager, Portfolio& ledger,
                          risk::RiskEngine& risk_engine, std::int64_t bid_price_units,
                          std::int64_t ask_price_units, std::uint64_t& next_exchange_order_id) {
  const std::int64_t limit_price_units = leg.side == Side::kBuy ? ask_price_units : bid_price_units;
  const auto client_order_id =
      manager.submit_new_order(leg.instrument_id, /*participant_id=*/1, leg.side, OrderType::kLimit,
                               limit_price_units, leg.quantity_units);
  const auto* tracked = manager.find_by_client_order_id(client_order_id);
  if (tracked == nullptr || tracked->lifecycle.state() == OrderState::kRejected) {
    risk_engine.on_order_rejected(client_order_id);
    return 0;
  }
  const std::int64_t approved_quantity_units = tracked->original_quantity_units;

  const std::uint64_t our_order_id = ++next_exchange_order_id;
  manager.handle_order_accepted(OrderAcceptedEvent{.order_id = our_order_id,
                                                   .instrument_id = leg.instrument_id,
                                                   .participant_id = 1,
                                                   .client_order_id = client_order_id,
                                                   .side = leg.side,
                                                   .order_type = OrderType::kLimit,
                                                   .price_units = limit_price_units,
                                                   .quantity_units = approved_quantity_units});

  // Our own order is always the trade's taker in this synthesis (see the
  // original design note this function inherits): kCounterpartyOrderId (0,
  // never assigned to a real tracked order -- exchange order ids here start
  // at 1) is the maker side, so apply_trade_to_portfolio's maker branch
  // never matches and only the taker branch applies the fill.
  constexpr std::uint64_t kCounterpartyOrderId = 0;
  const TradeEvent trade{
      .instrument_id = leg.instrument_id,
      .price_units = limit_price_units,
      .quantity_units = approved_quantity_units,
      .maker_order_id = kCounterpartyOrderId,
      .taker_order_id = our_order_id,
      .maker_participant_id = 0,
      .taker_participant_id = 1,
      .taker_side = leg.side,
  };
  manager.handle_trade(trade);
  apply_trade_to_portfolio(trade, manager, ledger, /*fee_units=*/0);
  risk_engine.on_fill(client_order_id, leg.instrument_id, leg.side, approved_quantity_units);

  manager.handle_order_terminated(OrderTerminatedEvent{.order_id = our_order_id,
                                                       .reason = TerminationReason::kFilled,
                                                       .cancelled_quantity_delta_units = 0});
  risk_engine.on_order_terminated(client_order_id);
  return client_order_id;
}

[[nodiscard]] std::string describe_calendar_spread_state(
    const CalendarSpreadStrategy& strategy_state, const StrategyProposal& proposal,
    const OrderManager& manager, const Portfolio& ledger, std::uint32_t near_instrument_id,
    std::int64_t near_mark_price_units, std::int64_t far_mark_price_units,
    const risk::ProposalRiskDecision* proposal_decision) {
  Json out;
  out["spread_price"] = proposal.spread_price;
  out["z_score"] = proposal.z_score;
  out["position"] = static_cast<int>(strategy_state.position());
  out["has_action"] = proposal.has_action;
  if (proposal.has_action) {
    out["near_side"] = static_cast<int>(proposal.near.side);
    out["far_side"] = static_cast<int>(proposal.far.side);
    out["quantity_units"] = proposal.near.quantity_units;
  }

  if (proposal_decision != nullptr) {
    Json risk_json;
    risk_json["proposal_id"] = proposal_decision->proposal_id;
    risk_json["verdict"] = static_cast<int>(proposal_decision->verdict);
    risk_json["reason_code"] = static_cast<int>(proposal_decision->reason_code);
    risk_json["reason"] = proposal_decision->reason;
    out["risk_decision"] = risk_json;
  }

  Json positions = Json::array();
  for (const auto& [instrument_id, position] : ledger.all_positions()) {
    const std::int64_t mark =
        instrument_id == near_instrument_id ? near_mark_price_units : far_mark_price_units;
    positions.push_back(Json{
        {"instrument_id", instrument_id},
        {"quantity_units", position.quantity_units},
        {"average_price_units", position.average_price_units},
        {"realized_pnl_units", position.realized_pnl_units},
        {"unrealized_pnl_units", ledger.unrealized_pnl_units(instrument_id, mark)},
    });
  }
  out["positions"] = positions;
  out["cash_units"] = ledger.cash_units();
  out["order_count"] = manager.all_tracked_orders().size();
  return out.dump();
}

}  // namespace

CalendarSpreadRunResult run_calendar_spread_scenario(const std::string& stream_path,
                                                     const CalendarSpreadRunConfig& config,
                                                     const risk::RiskLimitsConfig& risk_config) {
  const auto steps = read_market_data_steps(stream_path);

  feed::FeedHandler handler;
  book::BookBuilder near_book(config.near_instrument_id);
  book::BookBuilder far_book(config.far_instrument_id);

  const CalendarSpreadConfig strategy_config{.near_instrument_id = config.near_instrument_id,
                                             .far_instrument_id = config.far_instrument_id,
                                             .zscore_window = config.zscore_window,
                                             .entry_threshold = config.entry_threshold,
                                             .exit_threshold = config.exit_threshold,
                                             .quantity_units = config.quantity_units};
  CalendarSpreadStrategy strategy_state(strategy_config);

  risk::RiskEngine risk_engine(risk_config);
  common::ManualClock clock;
  RiskEngineGate risk_gate(risk_engine, clock);
  ImmediateFillExecutionAdapter immediate_fill_adapter;
  // A submission failure automatically releases the reservation decide_order
  // created -- ADR-0027's carry-in fix. ImmediateFillExecutionAdapter never
  // actually returns false, so this changes no observable demo behaviour;
  // it is here so the production composition is correct if that ever
  // changes, not merely so a test can exercise it.
  RiskReleasingExecutionAdapter adapter(immediate_fill_adapter, risk_engine);
  const oms::FeeSchedule fees{.fee_rate_ppm = 0};
  OrderManager manager(adapter, risk_gate, fees);
  Portfolio ledger;
  std::uint64_t next_exchange_order_id = 0;
  std::uint64_t proposal_sequence = 0;
  const std::string strategy_id = "calendar_spread";

  // Deterministic virtual time: one second per market-data step, injected
  // through a ManualClock (never the system clock -- AEGIS-005), so
  // staleness/rate-limit/collar checks and every audit-log timestamp are
  // reproducible byte-for-byte across runs.
  constexpr common::Duration kStepAdvance{1'000'000'000};

  CalendarSpreadRunResult result;
  bool near_ready = false;
  std::int64_t near_bid = 0;
  std::int64_t near_ask = 0;

  for (std::uint64_t sequence = 0; sequence < steps.size(); ++sequence) {
    clock.advance(kStepAdvance);
    const MarketDataStep& step = steps[sequence];
    const BookSnapshotEvent snapshot = make_two_sided_snapshot(step);
    const auto decoded = handler.decode(
        frame(MessageType::kBookSnapshot, sequence + 1, events::market_data::encode(snapshot)));
    if (!decoded.snapshot.has_value()) {
      continue;
    }

    if (step.leg == "near") {
      near_book.apply_snapshot(*decoded.snapshot);
      near_ready = true;
      near_bid = step.bid_price_units;
      near_ask = step.ask_price_units;
      risk_engine.on_market_data(config.near_instrument_id, (near_bid + near_ask) / 2, clock.now_utc(),
                                 /*valid=*/true);
      continue;  // Wait for this date's far leg before acting (header).
    }

    far_book.apply_snapshot(*decoded.snapshot);
    const std::int64_t far_bid = step.bid_price_units;
    const std::int64_t far_ask = step.ask_price_units;
    risk_engine.on_market_data(config.far_instrument_id, (far_bid + far_ask) / 2, clock.now_utc(),
                               /*valid=*/true);
    if (!near_ready) {
      continue;  // A far leg with no near observation yet: nothing to spread against.
    }

    const StrategyProposal proposal =
        strategy_state.on_book_update(near_book.top_of_book(), far_book.top_of_book());

    const risk::ProposalRiskDecision* decision_ptr = nullptr;
    if (proposal.has_action) {
      ++proposal_sequence;
      const std::string proposal_id = strategy_id + "-" + std::to_string(proposal_sequence);
      const std::int64_t near_price_units = proposal.near.side == Side::kBuy ? near_ask : near_bid;
      const std::int64_t far_price_units = proposal.far.side == Side::kBuy ? far_ask : far_bid;
      const std::vector<risk::OrderRequest> legs{
          risk::OrderRequest{.strategy_id = strategy_id,
                             .proposal_id = proposal_id,
                             .leg_index = 0,
                             .instrument_id = proposal.near.instrument_id,
                             .side = proposal.near.side,
                             .price_units = near_price_units,
                             .quantity_units = proposal.near.quantity_units,
                             .request_time_nanos = clock.now_utc()},
          risk::OrderRequest{.strategy_id = strategy_id,
                             .proposal_id = proposal_id,
                             .leg_index = 1,
                             .instrument_id = proposal.far.instrument_id,
                             .side = proposal.far.side,
                             .price_units = far_price_units,
                             .quantity_units = proposal.far.quantity_units,
                             .request_time_nanos = clock.now_utc()},
      };
      // Atomic, whole-proposal decision BEFORE either leg reaches the OMS
      // (AEGIS-137/ADR-0027): a reject here means execute_leg is called for
      // NEITHER leg, so a rejected spread can never become a naked
      // one-legged position.
      const risk::ProposalRiskDecision& decision =
          risk_engine.commit_proposal_decision(strategy_id, proposal_id, legs, clock.now_utc());
      decision_ptr = &decision;
      if (decision.verdict != risk::RiskVerdict::kReject) {
        execute_leg(proposal.near, manager, ledger, risk_engine, near_bid, near_ask,
                   next_exchange_order_id);
        execute_leg(proposal.far, manager, ledger, risk_engine, far_bid, far_ask, next_exchange_order_id);
      }
    }

    const std::int64_t near_mark = (near_bid + near_ask) / 2;
    const std::int64_t far_mark = (far_bid + far_ask) / 2;
    const std::int64_t equity_units = config.starting_capital_units + ledger.cash_units() +
        ledger.unrealized_pnl_units(config.near_instrument_id, near_mark) +
        ledger.unrealized_pnl_units(config.far_instrument_id, far_mark);
    risk_engine.on_equity_update(equity_units);
    risk_engine.on_session_pnl_update(equity_units - config.starting_capital_units);

    result.lines.push_back(describe_calendar_spread_state(
        strategy_state, proposal, manager, ledger, config.near_instrument_id, near_mark, far_mark,
        decision_ptr));
  }

  return result;
}

namespace {

[[nodiscard]] std::unordered_map<std::uint32_t, std::int64_t> parse_instrument_int64_map(
    const Json& node) {
  std::unordered_map<std::uint32_t, std::int64_t> result;
  for (const auto& [key, value] : node.items()) {
    result[static_cast<std::uint32_t>(std::stoul(key))] = value.get<std::int64_t>();
  }
  return result;
}

}  // namespace

risk::RiskLimitsConfig load_risk_limits_config(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("cannot open risk config " + path);
  }
  Json doc;
  file >> doc;

  risk::RiskLimitsConfig config;
  config.base_currency = doc.value("base_currency", std::string{"USD"});

  if (doc.contains("order_quantity_limits")) {
    for (const auto& [key, value] : doc.at("order_quantity_limits").items()) {
      config.order_quantity_limits[static_cast<std::uint32_t>(std::stoul(key))] =
          risk::OrderQuantityLimit{.max_order_quantity_units = value.at("max_order_quantity_units").get<std::int64_t>(),
                                  .resize_on_breach = value.value("resize_on_breach", false)};
    }
  }
  if (doc.contains("position_limits")) {
    for (const auto& [key, value] : doc.at("position_limits").items()) {
      config.position_limits[static_cast<std::uint32_t>(std::stoul(key))] =
          risk::PositionLimit{.max_long_units = value.at("max_long_units").get<std::int64_t>(),
                             .max_short_units = value.at("max_short_units").get<std::int64_t>()};
    }
  }
  config.max_order_notional_units = doc.value("max_order_notional_units", std::int64_t{0});
  config.max_portfolio_notional_units = doc.value("max_portfolio_notional_units", std::int64_t{0});
  if (doc.contains("fx_rate_to_base")) {
    for (const auto& [key, value] : doc.at("fx_rate_to_base").items()) {
      config.fx_rate_to_base[key] = value.get<double>();
    }
  }
  if (doc.contains("instruments")) {
    for (const auto& [key, value] : doc.at("instruments").items()) {
      config.instruments[static_cast<std::uint32_t>(std::stoul(key))] =
          risk::InstrumentInfo{.multiplier_units = value.value("multiplier_units", std::int64_t{1}),
                              .currency = value.value("currency", std::string{"USD"}),
                              .market = value.value("market", std::string{}),
                              .sector = value.value("sector", std::string{})};
    }
  }
  if (doc.contains("market_exposure_limit_units")) {
    for (const auto& [key, value] : doc.at("market_exposure_limit_units").items()) {
      config.market_exposure_limit_units[key] = value.get<std::int64_t>();
    }
  }
  if (doc.contains("sector_exposure_limit_units")) {
    for (const auto& [key, value] : doc.at("sector_exposure_limit_units").items()) {
      config.sector_exposure_limit_units[key] = value.get<std::int64_t>();
    }
  }
  config.price_collar_bps = doc.value("price_collar_bps", std::int64_t{0});
  config.max_quote_age = common::Duration{doc.value("max_quote_age_nanos", std::int64_t{0})};
  config.rate_limit_window =
      common::Duration{doc.value("rate_limit_window_nanos", std::int64_t{1'000'000'000})};
  config.max_orders_per_window = doc.value("max_orders_per_window", std::uint32_t{0});
  config.max_cancels_per_window = doc.value("max_cancels_per_window", std::uint32_t{0});
  if (doc.contains("margin_per_contract_units")) {
    config.margin.margin_per_contract_units =
        parse_instrument_int64_map(doc.at("margin_per_contract_units"));
  }
  config.max_leverage = doc.value("max_leverage", 0.0);
  config.daily_loss_limit_units = doc.value("daily_loss_limit_units", std::int64_t{0});
  config.max_drawdown_units = doc.value("max_drawdown_units", std::int64_t{0});
  if (doc.contains("volatility")) {
    const auto& vol = doc.at("volatility");
    config.volatility.window = vol.value("window", std::size_t{20});
    config.volatility.target_volatility = vol.value("target_volatility", 0.0);
    config.volatility.hard_reject_multiple = vol.value("hard_reject_multiple", 3.0);
  }
  if (doc.contains("concentration")) {
    const auto& conc = doc.at("concentration");
    config.concentration.max_concentration_share = conc.value("max_concentration_share", 1.0);
    if (conc.contains("correlated_groups")) {
      for (const auto& [group_id, members] : conc.at("correlated_groups").items()) {
        std::vector<std::uint32_t> ids;
        for (const auto& member : members) {
          ids.push_back(member.get<std::uint32_t>());
        }
        config.concentration.correlated_groups[group_id] = std::move(ids);
      }
    }
    if (conc.contains("group_exposure_limit_units")) {
      for (const auto& [group_id, value] : conc.at("group_exposure_limit_units").items()) {
        config.concentration.group_exposure_limit_units[group_id] = value.get<std::int64_t>();
      }
    }
  }
  return config;
}

}  // namespace aegis::participant::app
