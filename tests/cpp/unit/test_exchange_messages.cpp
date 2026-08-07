#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cpp/common/config.hpp"
#include "cpp/events/envelope.hpp"
#include "cpp/events/exchange_messages.hpp"

namespace {

using namespace aegis::events::exchange;  // NOLINT(google-build-using-namespace) - test-local
                                          // brevity
using aegis::events::MessageType;

nlohmann::json golden() {
  const std::filesystem::path root{AEGIS_SOURCE_ROOT};
  return aegis::common::load_schema(root / "tests/unit/fixtures/exchange_messages/golden.json");
}

Side parse_side(const std::string& value) { return value == "BUY" ? Side::kBuy : Side::kSell; }
OrderType parse_order_type(const std::string& value) {
  return value == "LIMIT" ? OrderType::kLimit : OrderType::kMarket;
}
RejectReason parse_reject_reason(const std::string& value) {
  static const std::unordered_map<std::string, RejectReason> by_name{
      {"UNKNOWN_INSTRUMENT", RejectReason::kUnknownInstrument},
      {"DUPLICATE_CLIENT_ORDER_ID", RejectReason::kDuplicateClientOrderId},
      {"NON_POSITIVE_QUANTITY", RejectReason::kNonPositiveQuantity},
      {"QUANTITY_NOT_ON_LOT", RejectReason::kQuantityNotOnLot},
      {"QUANTITY_OUT_OF_RANGE", RejectReason::kQuantityOutOfRange},
      {"PRICE_NOT_ON_TICK", RejectReason::kPriceNotOnTick},
      {"PRICE_OUT_OF_BAND", RejectReason::kPriceOutOfBand},
      {"PRICE_ON_MARKET_ORDER", RejectReason::kPriceOnMarketOrder},
      {"UNKNOWN_ORDER_ID", RejectReason::kUnknownOrderId},
      {"NOT_ORDER_OWNER", RejectReason::kNotOrderOwner},
      {"MODIFY_BELOW_FILLED", RejectReason::kModifyBelowFilled},
      {"MALFORMED_MESSAGE", RejectReason::kMalformedMessage},
  };
  return by_name.at(value);
}
TerminationReason parse_termination_reason(const std::string& value) {
  static const std::unordered_map<std::string, TerminationReason> by_name{
      {"FILLED", TerminationReason::kFilled},
      {"CANCELED", TerminationReason::kCanceled},
      {"RESIDUAL_CANCELED", TerminationReason::kResidualCanceled},
      {"REPLACED", TerminationReason::kReplaced},
  };
  return by_name.at(value);
}

/// Round-trips one golden case through the encode/decode pair its
/// `message_type` names, asserting both directions against the committed hex.
void check_case(const nlohmann::json& entry) {
  const auto name = entry.at("name").get<std::string>();
  const auto message_type = entry.at("message_type").get<std::string>();
  const auto& f = entry.at("fields");
  const auto expected_hex = entry.at("encoded_hex").get<std::string>();

  if (message_type == "kNewOrder") {
    const NewOrderCommand command{
        .instrument_id = f.at("instrument_id").get<std::uint32_t>(),
        .participant_id = f.at("participant_id").get<std::uint64_t>(),
        .client_order_id = f.at("client_order_id").get<std::uint64_t>(),
        .side = parse_side(f.at("side").get<std::string>()),
        .order_type = parse_order_type(f.at("order_type").get<std::string>()),
        .price_units = f.at("price_units").get<std::int64_t>(),
        .quantity_units = f.at("quantity_units").get<std::int64_t>(),
    };
    EXPECT_EQ(aegis::events::to_hex(encode(command)), expected_hex) << name;
    const auto decoded = decode_new_order(aegis::events::from_hex(expected_hex));
    ASSERT_TRUE(decoded.has_value()) << name;
    EXPECT_EQ(decoded, command) << name;
  } else if (message_type == "kCancelOrder") {
    const CancelOrderCommand command{
        .instrument_id = f.at("instrument_id").get<std::uint32_t>(),
        .participant_id = f.at("participant_id").get<std::uint64_t>(),
        .order_id = f.at("order_id").get<std::uint64_t>(),
    };
    EXPECT_EQ(aegis::events::to_hex(encode(command)), expected_hex) << name;
    const auto decoded = decode_cancel_order(aegis::events::from_hex(expected_hex));
    ASSERT_TRUE(decoded.has_value()) << name;
    EXPECT_EQ(decoded, command) << name;
  } else if (message_type == "kModifyOrder") {
    const ModifyOrderCommand command{
        .instrument_id = f.at("instrument_id").get<std::uint32_t>(),
        .participant_id = f.at("participant_id").get<std::uint64_t>(),
        .order_id = f.at("order_id").get<std::uint64_t>(),
        .new_price_units = f.at("new_price_units").get<std::int64_t>(),
        .new_quantity_units = f.at("new_quantity_units").get<std::int64_t>(),
    };
    EXPECT_EQ(aegis::events::to_hex(encode(command)), expected_hex) << name;
    const auto decoded = decode_modify_order(aegis::events::from_hex(expected_hex));
    ASSERT_TRUE(decoded.has_value()) << name;
    EXPECT_EQ(decoded, command) << name;
  } else if (message_type == "kOrderAccepted") {
    const OrderAcceptedEvent event{
        .causing_command_sequence = f.at("causing_command_sequence").get<std::uint64_t>(),
        .order_id = f.at("order_id").get<std::uint64_t>(),
        .instrument_id = f.at("instrument_id").get<std::uint32_t>(),
        .participant_id = f.at("participant_id").get<std::uint64_t>(),
        .client_order_id = f.at("client_order_id").get<std::uint64_t>(),
        .side = parse_side(f.at("side").get<std::string>()),
        .order_type = parse_order_type(f.at("order_type").get<std::string>()),
        .price_units = f.at("price_units").get<std::int64_t>(),
        .quantity_units = f.at("quantity_units").get<std::int64_t>(),
    };
    EXPECT_EQ(aegis::events::to_hex(encode(event)), expected_hex) << name;
    const auto decoded = decode_order_accepted(aegis::events::from_hex(expected_hex));
    ASSERT_TRUE(decoded.has_value()) << name;
    EXPECT_EQ(decoded, event) << name;
  } else if (message_type == "kOrderRejected") {
    const OrderRejectedEvent event{
        .causing_command_sequence = f.at("causing_command_sequence").get<std::uint64_t>(),
        .instrument_id = f.at("instrument_id").get<std::uint32_t>(),
        .participant_id = f.at("participant_id").get<std::uint64_t>(),
        .client_order_id = f.at("client_order_id").get<std::uint64_t>(),
        .reason = parse_reject_reason(f.at("reason").get<std::string>()),
    };
    EXPECT_EQ(aegis::events::to_hex(encode(event)), expected_hex) << name;
    const auto decoded = decode_order_rejected(aegis::events::from_hex(expected_hex));
    ASSERT_TRUE(decoded.has_value()) << name;
    EXPECT_EQ(decoded, event) << name;
  } else if (message_type == "kOrderModified") {
    const OrderModifiedEvent event{
        .causing_command_sequence = f.at("causing_command_sequence").get<std::uint64_t>(),
        .order_id = f.at("order_id").get<std::uint64_t>(),
        .new_remaining_units = f.at("new_remaining_units").get<std::int64_t>(),
        .cancelled_quantity_delta_units =
            f.at("cancelled_quantity_delta_units").get<std::int64_t>(),
    };
    EXPECT_EQ(aegis::events::to_hex(encode(event)), expected_hex) << name;
    const auto decoded = decode_order_modified(aegis::events::from_hex(expected_hex));
    ASSERT_TRUE(decoded.has_value()) << name;
    EXPECT_EQ(decoded, event) << name;
  } else if (message_type == "kOrderReplaced") {
    const OrderReplacedEvent event{
        .causing_command_sequence = f.at("causing_command_sequence").get<std::uint64_t>(),
        .old_order_id = f.at("old_order_id").get<std::uint64_t>(),
        .new_order_id = f.at("new_order_id").get<std::uint64_t>(),
        .instrument_id = f.at("instrument_id").get<std::uint32_t>(),
        .participant_id = f.at("participant_id").get<std::uint64_t>(),
        .client_order_id = f.at("client_order_id").get<std::uint64_t>(),
        .side = parse_side(f.at("side").get<std::string>()),
        .order_type = parse_order_type(f.at("order_type").get<std::string>()),
        .price_units = f.at("price_units").get<std::int64_t>(),
        .quantity_units = f.at("quantity_units").get<std::int64_t>(),
    };
    EXPECT_EQ(aegis::events::to_hex(encode(event)), expected_hex) << name;
    const auto decoded = decode_order_replaced(aegis::events::from_hex(expected_hex));
    ASSERT_TRUE(decoded.has_value()) << name;
    EXPECT_EQ(decoded, event) << name;
  } else if (message_type == "kTrade") {
    const TradeEvent event{
        .causing_command_sequence = f.at("causing_command_sequence").get<std::uint64_t>(),
        .instrument_id = f.at("instrument_id").get<std::uint32_t>(),
        .price_units = f.at("price_units").get<std::int64_t>(),
        .quantity_units = f.at("quantity_units").get<std::int64_t>(),
        .maker_order_id = f.at("maker_order_id").get<std::uint64_t>(),
        .taker_order_id = f.at("taker_order_id").get<std::uint64_t>(),
        .maker_participant_id = f.at("maker_participant_id").get<std::uint64_t>(),
        .taker_participant_id = f.at("taker_participant_id").get<std::uint64_t>(),
        .taker_side = parse_side(f.at("taker_side").get<std::string>()),
    };
    EXPECT_EQ(aegis::events::to_hex(encode(event)), expected_hex) << name;
    const auto decoded = decode_trade(aegis::events::from_hex(expected_hex));
    ASSERT_TRUE(decoded.has_value()) << name;
    EXPECT_EQ(decoded, event) << name;
  } else if (message_type == "kOrderTerminated") {
    const OrderTerminatedEvent event{
        .causing_command_sequence = f.at("causing_command_sequence").get<std::uint64_t>(),
        .order_id = f.at("order_id").get<std::uint64_t>(),
        .reason = parse_termination_reason(f.at("reason").get<std::string>()),
        .cancelled_quantity_delta_units =
            f.at("cancelled_quantity_delta_units").get<std::int64_t>(),
    };
    EXPECT_EQ(aegis::events::to_hex(encode(event)), expected_hex) << name;
    const auto decoded = decode_order_terminated(aegis::events::from_hex(expected_hex));
    ASSERT_TRUE(decoded.has_value()) << name;
    EXPECT_EQ(decoded, event) << name;
  } else {
    FAIL() << "unrecognised message_type in golden fixture: " << message_type;
  }
}

TEST(ExchangeMessages, MatchTheGoldenBytesSharedWithPython) {
  const auto document = golden();
  ASSERT_FALSE(document.at("cases").empty()) << "an empty fixture asserts nothing";
  for (const auto& entry : document.at("cases")) {
    check_case(entry);
  }
}

TEST(ExchangeMessages, MessageTypeNumbersArePermanent) {
  EXPECT_EQ(static_cast<std::uint16_t>(MessageType::kNewOrder), 1);
  EXPECT_EQ(static_cast<std::uint16_t>(MessageType::kCancelOrder), 2);
  EXPECT_EQ(static_cast<std::uint16_t>(MessageType::kModifyOrder), 3);
  EXPECT_EQ(static_cast<std::uint16_t>(MessageType::kOrderAccepted), 10);
  EXPECT_EQ(static_cast<std::uint16_t>(MessageType::kOrderRejected), 11);
  EXPECT_EQ(static_cast<std::uint16_t>(MessageType::kOrderModified), 12);
  EXPECT_EQ(static_cast<std::uint16_t>(MessageType::kOrderReplaced), 13);
  EXPECT_EQ(static_cast<std::uint16_t>(MessageType::kTrade), 14);
  EXPECT_EQ(static_cast<std::uint16_t>(MessageType::kOrderTerminated), 15);
}

TEST(ExchangeMessages, UnknownMessageTypeStaysUnknown) {
  EXPECT_FALSE(
      aegis::events::is_known_message_type(4));  // reserved gap between commands and events
  EXPECT_FALSE(aegis::events::is_known_message_type(9));  // reserved gap before event numbers start
  EXPECT_FALSE(aegis::events::is_known_message_type(16));
}

TEST(ExchangeMessages, TruncatedPayloadFailsToDecode) {
  const NewOrderCommand command{.instrument_id = 1,
                                .participant_id = 2,
                                .client_order_id = 3,
                                .side = Side::kBuy,
                                .order_type = OrderType::kLimit,
                                .price_units = 100,
                                .quantity_units = 10};
  auto bytes = encode(command);
  bytes.pop_back();
  EXPECT_FALSE(decode_new_order(bytes).has_value());
}

TEST(ExchangeMessages, TrailingBytesFailToDecode) {
  const CancelOrderCommand command{.instrument_id = 1, .participant_id = 2, .order_id = 3};
  auto bytes = encode(command);
  bytes.push_back(std::byte{0});
  EXPECT_FALSE(decode_cancel_order(bytes).has_value());
}

TEST(ExchangeMessages, UnknownRejectReasonFailsToDecode) {
  const OrderRejectedEvent event{.causing_command_sequence = 1,
                                 .instrument_id = 2,
                                 .participant_id = 3,
                                 .client_order_id = 4,
                                 .reason = RejectReason::kMalformedMessage};
  auto bytes = encode(event);
  bytes.back() = std::byte{200};  // not a defined RejectReason
  EXPECT_FALSE(decode_order_rejected(bytes).has_value());
}

TEST(ExchangeMessages, EveryRejectReasonAndTerminationReasonHasADescription) {
  for (const auto reason : {RejectReason::kUnknownInstrument, RejectReason::kDuplicateClientOrderId,
                            RejectReason::kNonPositiveQuantity, RejectReason::kQuantityNotOnLot,
                            RejectReason::kQuantityOutOfRange, RejectReason::kPriceNotOnTick,
                            RejectReason::kPriceOutOfBand, RejectReason::kPriceOnMarketOrder,
                            RejectReason::kUnknownOrderId, RejectReason::kNotOrderOwner,
                            RejectReason::kModifyBelowFilled, RejectReason::kMalformedMessage}) {
    EXPECT_FALSE(describe(reason).empty());
  }
  for (const auto reason : {TerminationReason::kFilled, TerminationReason::kCanceled,
                            TerminationReason::kResidualCanceled, TerminationReason::kReplaced}) {
    EXPECT_FALSE(describe(reason).empty());
  }
}

}  // namespace
