#include <array>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/exchange/app/exchange_node.hpp"
#include "cpp/exchange/state/snapshot.hpp"

/// ADR-0013: byte-stable snapshot codec, both counter-consistency refusals,
/// `write == restore -> write`, and continuation equality — a run split as
/// *first half -> snapshot -> restore into a fresh `ExchangeNode` -> second
/// half* must emit byte-identical canonical output to the same commands run
/// uninterrupted, including every `EventSequence` and every `OrderId`
/// assigned after the split.
namespace {

using aegis::common::EventTime;
using aegis::common::ExchangeTime;
using aegis::events::CommandSequence;
using aegis::events::EventSequence;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::exchange::capture_snapshot;
using aegis::exchange::ExchangeNode;
using aegis::exchange::ExchangeSnapshot;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::OrderBook;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;
using aegis::exchange::read_snapshot;
using aegis::exchange::restore_orders_into;
using aegis::exchange::SnapshotError;
using aegis::exchange::SnapshotOrderRecord;
using aegis::exchange::write_snapshot;

InstrumentSpec make_spec() {
  InstrumentSpec spec;
  spec.instrument_id = InstrumentId{1};
  spec.price_floor_units = PriceUnits{1000};
  spec.price_ceiling_units = PriceUnits{2000};
  spec.tick_size_units = 25;
  spec.min_quantity_units = QuantityUnits{50};
  spec.max_quantity_units = QuantityUnits{5000};
  spec.lot_size_units = 50;
  return spec;
}

void log_new(ExchangeNode& node, const NewOrderCommand& command, EventTime time) {
  const auto command_sequence = node.sequencer().sequence(time);
  for (auto& event : node.apply_new_order(command, command_sequence)) {
    node.event_log().append(event.message_type, std::move(event.payload), time);
  }
}

void log_cancel(ExchangeNode& node, const CancelOrderCommand& command, EventTime time) {
  const auto command_sequence = node.sequencer().sequence(time);
  for (auto& event : node.apply_cancel_order(command, command_sequence)) {
    node.event_log().append(event.message_type, std::move(event.payload), time);
  }
}

void log_modify(ExchangeNode& node, const ModifyOrderCommand& command, EventTime time) {
  const auto command_sequence = node.sequencer().sequence(time);
  for (auto& event : node.apply_modify_order(command, command_sequence)) {
    node.event_log().append(event.message_type, std::move(event.payload), time);
  }
}

/// Orders 1-3 rest, order 1 is replaced by a quantity increase (id 4), order
/// 3 is cancelled, and a crossing order (id 5) partially fills the resting
/// sell — leaving id 4 (buy@1000, 150) and id 2 (sell@1200, 50 remaining)
/// live. `next_order_id` is 6, `next_command_sequence` is 7 afterward.
void run_first_half(ExchangeNode& node) {
  log_new(node,
          NewOrderCommand{.instrument_id = 1,
                          .participant_id = 1,
                          .client_order_id = 1,
                          .side = Side::kBuy,
                          .order_type = OrderType::kLimit,
                          .price_units = 1000,
                          .quantity_units = 100},
          EventTime{0});
  log_new(node,
          NewOrderCommand{.instrument_id = 1,
                          .participant_id = 1,
                          .client_order_id = 2,
                          .side = Side::kSell,
                          .order_type = OrderType::kLimit,
                          .price_units = 1200,
                          .quantity_units = 150},
          EventTime{1});
  log_new(node,
          NewOrderCommand{.instrument_id = 1,
                          .participant_id = 2,
                          .client_order_id = 1,
                          .side = Side::kBuy,
                          .order_type = OrderType::kLimit,
                          .price_units = 1100,
                          .quantity_units = 200},
          EventTime{2});
  // Price unchanged, quantity increased: cancel-replace (id 1 -> id 4).
  log_modify(node,
             ModifyOrderCommand{.instrument_id = 1,
                                .participant_id = 1,
                                .order_id = 1,
                                .new_price_units = 1000,
                                .new_quantity_units = 150},
             EventTime{3});
  log_cancel(node, CancelOrderCommand{.instrument_id = 1, .participant_id = 2, .order_id = 3},
             EventTime{4});
  // Crosses the resting sell (id 2) for 100 of its 150.
  log_new(node,
          NewOrderCommand{.instrument_id = 1,
                          .participant_id = 3,
                          .client_order_id = 1,
                          .side = Side::kBuy,
                          .order_type = OrderType::kLimit,
                          .price_units = 1200,
                          .quantity_units = 100},
          EventTime{5});
}

/// Continues from wherever `node` is: a price-change replace of id 4 (-> id
/// 6), a fully crossing new sell that empties the book of resting buys, a
/// cancel of the remaining sell (id 2), and a market order against an empty
/// book (residual cancelled). Exercises every event type across the split.
void run_second_half(ExchangeNode& node) {
  log_modify(node,
             ModifyOrderCommand{.instrument_id = 1,
                                .participant_id = 1,
                                .order_id = 4,
                                .new_price_units = 1050,
                                .new_quantity_units = 150},
             EventTime{6});
  log_new(node,
          NewOrderCommand{.instrument_id = 1,
                          .participant_id = 4,
                          .client_order_id = 1,
                          .side = Side::kSell,
                          .order_type = OrderType::kLimit,
                          .price_units = 1050,
                          .quantity_units = 150},
          EventTime{7});
  log_cancel(node, CancelOrderCommand{.instrument_id = 1, .participant_id = 1, .order_id = 2},
             EventTime{8});
  log_new(node,
          NewOrderCommand{.instrument_id = 1,
                          .participant_id = 5,
                          .client_order_id = 1,
                          .side = Side::kBuy,
                          .order_type = OrderType::kMarket,
                          .price_units = 0,
                          .quantity_units = 100},
          EventTime{9});
}

ExchangeSnapshot make_sample_snapshot() {
  ExchangeSnapshot snapshot;
  snapshot.next_command_sequence = 100;
  snapshot.next_event_sequence = 50;
  snapshot.next_order_id = 30;
  snapshot.last_exchange_time_nanos = 123456789;
  snapshot.orders.push_back(SnapshotOrderRecord{.order_id = 10,
                                                .instrument_id = 1,
                                                .participant_id = 1,
                                                .client_order_id = 1,
                                                .side = Side::kBuy,
                                                .order_type = OrderType::kLimit,
                                                .price_units = 1000,
                                                .original_quantity = 150,
                                                .cumulative_filled = 50,
                                                .cancelled_quantity = 0,
                                                .remaining = 100,
                                                .priority_command_sequence = 20});
  snapshot.orders.push_back(SnapshotOrderRecord{.order_id = 15,
                                                .instrument_id = 1,
                                                .participant_id = 2,
                                                .client_order_id = 1,
                                                .side = Side::kSell,
                                                .order_type = OrderType::kLimit,
                                                .price_units = 1100,
                                                .original_quantity = 200,
                                                .cumulative_filled = 0,
                                                .cancelled_quantity = 0,
                                                .remaining = 200,
                                                .priority_command_sequence = 25});
  return snapshot;
}

TEST(SnapshotRoundtrip, WriteIsByteStableAcrossRepeatedCalls) {
  const auto snapshot = make_sample_snapshot();
  EXPECT_EQ(write_snapshot(snapshot), write_snapshot(snapshot));
}

TEST(SnapshotRoundtrip, WriteEqualsRestoreThenWrite) {
  const auto snapshot = make_sample_snapshot();
  const auto bytes = write_snapshot(snapshot);
  const auto result = read_snapshot(bytes);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(write_snapshot(result.value_or(ExchangeSnapshot{})), bytes);
}

TEST(SnapshotRoundtrip, ReadRecoversEveryField) {
  const auto snapshot = make_sample_snapshot();
  const auto result = read_snapshot(write_snapshot(snapshot));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value_or(ExchangeSnapshot{}), snapshot);
}

TEST(SnapshotRoundtrip, UnknownVersionIsRefused) {
  auto snapshot = make_sample_snapshot();
  snapshot.snapshot_version = 2;
  const auto result = read_snapshot(write_snapshot(snapshot));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SnapshotError::kUnknownVersion);
}

TEST(SnapshotRoundtrip, OrderIdAtOrBelowNextOrderIdIsRefused) {
  auto snapshot = make_sample_snapshot();
  snapshot.next_order_id = snapshot.orders.front().order_id;  // must be strictly greater
  const auto result = read_snapshot(write_snapshot(snapshot));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SnapshotError::kCounterInconsistent);
}

TEST(SnapshotRoundtrip, CommandSequenceAtOrBelowPriorityIsRefused) {
  auto snapshot = make_sample_snapshot();
  snapshot.next_command_sequence = snapshot.orders.front().priority_command_sequence;
  const auto result = read_snapshot(write_snapshot(snapshot));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SnapshotError::kCounterInconsistent);
}

TEST(SnapshotRoundtrip, TruncatedBytesAreRefused) {
  auto bytes = write_snapshot(make_sample_snapshot());
  bytes.resize(bytes.size() - 1);
  const auto result = read_snapshot(bytes);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SnapshotError::kTruncated);
}

TEST(SnapshotRoundtrip, CaptureSnapshotOrdersAreInCanonicalOrder) {
  const auto spec = make_spec();
  ExchangeNode node;
  node.add_instrument(spec);
  run_first_half(node);

  const auto* book = node.book(InstrumentId{1});
  ASSERT_NE(book, nullptr);
  const std::array<const OrderBook*, 1> books{book};
  const auto snapshot =
      capture_snapshot(node.sequencer(), node.event_log(), node.next_order_id(), books);

  ASSERT_GE(snapshot.orders.size(), 2U);
  for (std::size_t i = 1; i < snapshot.orders.size(); ++i) {
    const auto& prev = snapshot.orders[i - 1];
    const auto& curr = snapshot.orders[i];
    const bool ordered =
        std::tie(prev.instrument_id, prev.price_units, prev.priority_command_sequence) <
        std::tie(curr.instrument_id, curr.price_units, curr.priority_command_sequence);
    EXPECT_TRUE(ordered) << "orders not in canonical order at index " << i;
  }
}

TEST(SnapshotRoundtrip, CaptureSnapshotCountersMatchTheLiveExchange) {
  const auto spec = make_spec();
  ExchangeNode node;
  node.add_instrument(spec);
  run_first_half(node);

  const auto* book = node.book(InstrumentId{1});
  ASSERT_NE(book, nullptr);
  const std::array<const OrderBook*, 1> books{book};
  const auto snapshot =
      capture_snapshot(node.sequencer(), node.event_log(), node.next_order_id(), books);

  EXPECT_EQ(snapshot.next_command_sequence, node.sequencer().next_command_sequence().value());
  EXPECT_EQ(snapshot.next_event_sequence, node.event_log().next_event_sequence().value());
  EXPECT_EQ(snapshot.next_order_id, node.next_order_id());
  EXPECT_EQ(snapshot.orders.size(), 2U);  // id 4 (buy@1000) and id 2 (sell@1200, 50 left)
}

TEST(SnapshotRoundtrip, ContinuationEqualityAcrossSnapshotRestore) {
  const auto spec = make_spec();

  // Run A: the whole sequence through one uninterrupted ExchangeNode.
  ExchangeNode node_a;
  node_a.add_instrument(spec);
  run_first_half(node_a);
  const auto first_half_event_count = node_a.event_log().next_event_sequence().value() - 1;
  run_second_half(node_a);
  const auto lines_a = node_a.event_log().to_canonical_lines();
  const std::vector<std::string> second_half_uninterrupted(
      lines_a.begin() + static_cast<std::ptrdiff_t>(first_half_event_count), lines_a.end());
  ASSERT_FALSE(second_half_uninterrupted.empty());

  // Run B: the first half through one node, snapshot, restore into a fresh
  // node, then the identical second half through the restored node.
  ExchangeNode node_b1;
  node_b1.add_instrument(spec);
  run_first_half(node_b1);

  const auto* book_b1 = node_b1.book(InstrumentId{1});
  ASSERT_NE(book_b1, nullptr);
  const std::array<const OrderBook*, 1> books_b1{book_b1};
  const auto snapshot =
      capture_snapshot(node_b1.sequencer(), node_b1.event_log(), node_b1.next_order_id(), books_b1);
  const auto read_result = read_snapshot(write_snapshot(snapshot));
  ASSERT_TRUE(read_result.has_value());
  const auto restored = read_result.value_or(ExchangeSnapshot{});

  ExchangeNode node_b2{CommandSequence{restored.next_command_sequence},
                       ExchangeTime{restored.last_exchange_time_nanos},
                       EventSequence{restored.next_event_sequence}, restored.next_order_id};
  node_b2.add_instrument(spec);
  auto* book_b2 = node_b2.book(InstrumentId{1});
  ASSERT_NE(book_b2, nullptr);
  restore_orders_into(*book_b2, InstrumentId{1}, restored);

  run_second_half(node_b2);
  const auto lines_b2 = node_b2.event_log().to_canonical_lines();

  EXPECT_EQ(second_half_uninterrupted, lines_b2);
}

}  // namespace
