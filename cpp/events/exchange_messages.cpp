#include "cpp/events/exchange_messages.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "cpp/events/wire.hpp"

namespace aegis::events::exchange {
namespace {

using wire::put_i64;
using wire::put_u32;
using wire::put_u64;
using wire::put_u8;
using wire::take_i64;
using wire::take_u32;
using wire::take_u64;
using wire::take_u8;

void put_side(std::vector<std::byte>& out, Side side) {
  put_u8(out, static_cast<std::uint8_t>(side));
}
void put_order_type(std::vector<std::byte>& out, OrderType type) {
  put_u8(out, static_cast<std::uint8_t>(type));
}

[[nodiscard]] bool take_side(std::span<const std::byte> bytes, std::size_t& offset, Side& out) {
  std::uint8_t raw{0};
  if (!take_u8(bytes, offset, raw) || raw > static_cast<std::uint8_t>(Side::kSell)) {
    return false;
  }
  out = static_cast<Side>(raw);
  return true;
}

[[nodiscard]] bool take_order_type(std::span<const std::byte> bytes, std::size_t& offset,
                                   OrderType& out) {
  std::uint8_t raw{0};
  if (!take_u8(bytes, offset, raw) || raw > static_cast<std::uint8_t>(OrderType::kMarket)) {
    return false;
  }
  out = static_cast<OrderType>(raw);
  return true;
}

}  // namespace

bool is_known_reject_reason(std::uint8_t value) {
  return value <= static_cast<std::uint8_t>(RejectReason::kMalformedMessage);
}

std::string_view describe(RejectReason reason) {
  switch (reason) {
    case RejectReason::kUnknownInstrument:
      return "instrument id is not known to this exchange";
    case RejectReason::kDuplicateClientOrderId:
      return "(participant_id, client_order_id) already names a live order";
    case RejectReason::kNonPositiveQuantity:
      return "quantity must be positive";
    case RejectReason::kQuantityNotOnLot:
      return "quantity is not a multiple of the instrument's lot size";
    case RejectReason::kQuantityOutOfRange:
      return "quantity is outside [min_quantity_units, max_quantity_units]";
    case RejectReason::kPriceNotOnTick:
      return "price is not on the instrument's tick grid";
    case RejectReason::kPriceOutOfBand:
      return "price is outside [price_floor_units, price_ceiling_units]";
    case RejectReason::kPriceOnMarketOrder:
      return "a market order must not carry a price";
    case RejectReason::kUnknownOrderId:
      return "order id is unknown, or names an order that has already terminated";
    case RejectReason::kNotOrderOwner:
      return "order is live but owned by a different participant";
    case RejectReason::kModifyBelowFilled:
      return "modify would set quantity below what has already been filled";
    case RejectReason::kMalformedMessage:
      return "message could not be decoded";
  }
  return "unknown reject reason";
}

bool is_known_termination_reason(std::uint8_t value) {
  return value <= static_cast<std::uint8_t>(TerminationReason::kReplaced);
}

std::string_view describe(TerminationReason reason) {
  switch (reason) {
    case TerminationReason::kFilled:
      return "fully filled";
    case TerminationReason::kCanceled:
      return "canceled by request";
    case TerminationReason::kResidualCanceled:
      return "unfilled residual of an accepted order canceled (market residual policy)";
    case TerminationReason::kReplaced:
      return "terminated by cancel-replace";
  }
  return "unknown termination reason";
}

// ------------------------------------------------------------------ commands

std::vector<std::byte> encode(const NewOrderCommand& command) {
  std::vector<std::byte> out;
  put_u32(out, command.instrument_id);
  put_u64(out, command.participant_id);
  put_u64(out, command.client_order_id);
  put_side(out, command.side);
  put_order_type(out, command.order_type);
  put_i64(out, command.price_units);
  put_i64(out, command.quantity_units);
  return out;
}

std::optional<NewOrderCommand> decode_new_order(std::span<const std::byte> bytes) {
  NewOrderCommand command;
  std::size_t offset = 0;
  if (!take_u32(bytes, offset, command.instrument_id) ||
      !take_u64(bytes, offset, command.participant_id) ||
      !take_u64(bytes, offset, command.client_order_id) ||
      !take_side(bytes, offset, command.side) ||
      !take_order_type(bytes, offset, command.order_type) ||
      !take_i64(bytes, offset, command.price_units) ||
      !take_i64(bytes, offset, command.quantity_units) || offset != bytes.size()) {
    return std::nullopt;
  }
  return command;
}

std::vector<std::byte> encode(const CancelOrderCommand& command) {
  std::vector<std::byte> out;
  put_u32(out, command.instrument_id);
  put_u64(out, command.participant_id);
  put_u64(out, command.order_id);
  return out;
}

std::optional<CancelOrderCommand> decode_cancel_order(std::span<const std::byte> bytes) {
  CancelOrderCommand command;
  std::size_t offset = 0;
  if (!take_u32(bytes, offset, command.instrument_id) ||
      !take_u64(bytes, offset, command.participant_id) ||
      !take_u64(bytes, offset, command.order_id) || offset != bytes.size()) {
    return std::nullopt;
  }
  return command;
}

std::vector<std::byte> encode(const ModifyOrderCommand& command) {
  std::vector<std::byte> out;
  put_u32(out, command.instrument_id);
  put_u64(out, command.participant_id);
  put_u64(out, command.order_id);
  put_i64(out, command.new_price_units);
  put_i64(out, command.new_quantity_units);
  return out;
}

std::optional<ModifyOrderCommand> decode_modify_order(std::span<const std::byte> bytes) {
  ModifyOrderCommand command;
  std::size_t offset = 0;
  if (!take_u32(bytes, offset, command.instrument_id) ||
      !take_u64(bytes, offset, command.participant_id) ||
      !take_u64(bytes, offset, command.order_id) ||
      !take_i64(bytes, offset, command.new_price_units) ||
      !take_i64(bytes, offset, command.new_quantity_units) || offset != bytes.size()) {
    return std::nullopt;
  }
  return command;
}

// -------------------------------------------------------------------- events

std::vector<std::byte> encode(const OrderAcceptedEvent& event) {
  std::vector<std::byte> out;
  put_u64(out, event.causing_command_sequence);
  put_u64(out, event.order_id);
  put_u32(out, event.instrument_id);
  put_u64(out, event.participant_id);
  put_u64(out, event.client_order_id);
  put_side(out, event.side);
  put_order_type(out, event.order_type);
  put_i64(out, event.price_units);
  put_i64(out, event.quantity_units);
  return out;
}

std::optional<OrderAcceptedEvent> decode_order_accepted(std::span<const std::byte> bytes) {
  OrderAcceptedEvent event;
  std::size_t offset = 0;
  if (!take_u64(bytes, offset, event.causing_command_sequence) ||
      !take_u64(bytes, offset, event.order_id) || !take_u32(bytes, offset, event.instrument_id) ||
      !take_u64(bytes, offset, event.participant_id) ||
      !take_u64(bytes, offset, event.client_order_id) || !take_side(bytes, offset, event.side) ||
      !take_order_type(bytes, offset, event.order_type) ||
      !take_i64(bytes, offset, event.price_units) ||
      !take_i64(bytes, offset, event.quantity_units) || offset != bytes.size()) {
    return std::nullopt;
  }
  return event;
}

std::vector<std::byte> encode(const OrderRejectedEvent& event) {
  std::vector<std::byte> out;
  put_u64(out, event.causing_command_sequence);
  put_u32(out, event.instrument_id);
  put_u64(out, event.participant_id);
  put_u64(out, event.client_order_id);
  put_u64(out, event.order_id);
  put_u8(out, static_cast<std::uint8_t>(event.reason));
  return out;
}

std::optional<OrderRejectedEvent> decode_order_rejected(std::span<const std::byte> bytes) {
  OrderRejectedEvent event;
  std::size_t offset = 0;
  std::uint8_t raw_reason{0};
  if (!take_u64(bytes, offset, event.causing_command_sequence) ||
      !take_u32(bytes, offset, event.instrument_id) ||
      !take_u64(bytes, offset, event.participant_id) ||
      !take_u64(bytes, offset, event.client_order_id) || !take_u64(bytes, offset, event.order_id) ||
      !take_u8(bytes, offset, raw_reason) || !is_known_reject_reason(raw_reason) ||
      offset != bytes.size()) {
    return std::nullopt;
  }
  event.reason = static_cast<RejectReason>(raw_reason);
  return event;
}

std::vector<std::byte> encode(const OrderModifiedEvent& event) {
  std::vector<std::byte> out;
  put_u64(out, event.causing_command_sequence);
  put_u64(out, event.order_id);
  put_i64(out, event.new_remaining_units);
  put_i64(out, event.cancelled_quantity_delta_units);
  return out;
}

std::optional<OrderModifiedEvent> decode_order_modified(std::span<const std::byte> bytes) {
  OrderModifiedEvent event;
  std::size_t offset = 0;
  if (!take_u64(bytes, offset, event.causing_command_sequence) ||
      !take_u64(bytes, offset, event.order_id) ||
      !take_i64(bytes, offset, event.new_remaining_units) ||
      !take_i64(bytes, offset, event.cancelled_quantity_delta_units) || offset != bytes.size()) {
    return std::nullopt;
  }
  return event;
}

std::vector<std::byte> encode(const OrderReplacedEvent& event) {
  std::vector<std::byte> out;
  put_u64(out, event.causing_command_sequence);
  put_u64(out, event.old_order_id);
  put_u64(out, event.new_order_id);
  put_u32(out, event.instrument_id);
  put_u64(out, event.participant_id);
  put_u64(out, event.client_order_id);
  put_side(out, event.side);
  put_order_type(out, event.order_type);
  put_i64(out, event.price_units);
  put_i64(out, event.quantity_units);
  return out;
}

std::optional<OrderReplacedEvent> decode_order_replaced(std::span<const std::byte> bytes) {
  OrderReplacedEvent event;
  std::size_t offset = 0;
  if (!take_u64(bytes, offset, event.causing_command_sequence) ||
      !take_u64(bytes, offset, event.old_order_id) ||
      !take_u64(bytes, offset, event.new_order_id) ||
      !take_u32(bytes, offset, event.instrument_id) ||
      !take_u64(bytes, offset, event.participant_id) ||
      !take_u64(bytes, offset, event.client_order_id) || !take_side(bytes, offset, event.side) ||
      !take_order_type(bytes, offset, event.order_type) ||
      !take_i64(bytes, offset, event.price_units) ||
      !take_i64(bytes, offset, event.quantity_units) || offset != bytes.size()) {
    return std::nullopt;
  }
  return event;
}

std::vector<std::byte> encode(const TradeEvent& event) {
  std::vector<std::byte> out;
  put_u64(out, event.causing_command_sequence);
  put_u32(out, event.instrument_id);
  put_i64(out, event.price_units);
  put_i64(out, event.quantity_units);
  put_u64(out, event.maker_order_id);
  put_u64(out, event.taker_order_id);
  put_u64(out, event.maker_participant_id);
  put_u64(out, event.taker_participant_id);
  put_side(out, event.taker_side);
  return out;
}

std::optional<TradeEvent> decode_trade(std::span<const std::byte> bytes) {
  TradeEvent event;
  std::size_t offset = 0;
  if (!take_u64(bytes, offset, event.causing_command_sequence) ||
      !take_u32(bytes, offset, event.instrument_id) ||
      !take_i64(bytes, offset, event.price_units) ||
      !take_i64(bytes, offset, event.quantity_units) ||
      !take_u64(bytes, offset, event.maker_order_id) ||
      !take_u64(bytes, offset, event.taker_order_id) ||
      !take_u64(bytes, offset, event.maker_participant_id) ||
      !take_u64(bytes, offset, event.taker_participant_id) ||
      !take_side(bytes, offset, event.taker_side) || offset != bytes.size()) {
    return std::nullopt;
  }
  return event;
}

std::vector<std::byte> encode(const OrderTerminatedEvent& event) {
  std::vector<std::byte> out;
  put_u64(out, event.causing_command_sequence);
  put_u64(out, event.order_id);
  put_u8(out, static_cast<std::uint8_t>(event.reason));
  put_i64(out, event.cancelled_quantity_delta_units);
  return out;
}

std::optional<OrderTerminatedEvent> decode_order_terminated(std::span<const std::byte> bytes) {
  OrderTerminatedEvent event;
  std::size_t offset = 0;
  std::uint8_t raw_reason{0};
  if (!take_u64(bytes, offset, event.causing_command_sequence) ||
      !take_u64(bytes, offset, event.order_id) || !take_u8(bytes, offset, raw_reason) ||
      !is_known_termination_reason(raw_reason) ||
      !take_i64(bytes, offset, event.cancelled_quantity_delta_units) || offset != bytes.size()) {
    return std::nullopt;
  }
  event.reason = static_cast<TerminationReason>(raw_reason);
  return event;
}

}  // namespace aegis::events::exchange
