#include "cpp/participant/app/participant_run.hpp"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "cpp/events/envelope.hpp"
#include "cpp/events/exchange_messages.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/participant/book_builder/book_builder.hpp"
#include "cpp/participant/feed_handler/feed_handler.hpp"
#include "cpp/participant/oms/order_lifecycle.hpp"
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

}  // namespace aegis::participant::app
