#include "cpp/participant/app/participant_run.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cpp/events/envelope.hpp"
#include "cpp/events/exchange_messages.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/participant/app/participant_snapshot.hpp"
#include "cpp/participant/book_builder/book_builder.hpp"
#include "cpp/participant/feed_handler/feed_handler.hpp"
#include "cpp/participant/oms/order_lifecycle.hpp"
#include "cpp/participant/oms/order_manager.hpp"
#include "cpp/participant/oms/recorded_response_adapter.hpp"
#include "cpp/participant/oms/risk_gate.hpp"
#include "cpp/participant/portfolio/portfolio.hpp"
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
                            snapshot.oms.next_client_order_id);
    ledger_storage.emplace(portfolio::restore_portfolio(snapshot.portfolio));
  } else {
    manager_storage.emplace(adapter, risk);
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

}  // namespace aegis::participant::app
