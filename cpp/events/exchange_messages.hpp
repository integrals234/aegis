#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/events/envelope.hpp"

/// Exchange domain message vocabulary (ADR-0009).
///
/// Lives in `cpp/events`, not `cpp/exchange`, so the M3 participant can decode
/// what the exchange publishes without depending on the exchange layer at all
/// (AEGIS-004 made structural). Every command a caller submits and every event
/// the matching engine emits has exactly one wire representation here: fixed
/// field order, fixed-width little-endian, length-prefixed strings, no
/// floating point (envelope.hpp's canonical rules, reused via
/// `cpp/events/wire.hpp`).
///
/// `MessageType` numbers are permanent (`cpp/events/envelope.hpp`). A command
/// carries no `CommandSequence` — the sequencer assigns one on receipt, so it
/// cannot be part of what the caller submits. An event's `causing_command_sequence`
/// is the `CommandSequence` of the command that produced it; combined with the
/// carrying `Envelope.sequence` (the event's own `EventSequence`), a consumer
/// can always answer "which command caused this, and where does it sit in the
/// canonical output stream".
namespace aegis::events::exchange {

// ---------------------------------------------------------------- vocabulary

/// NOLINTNEXTLINE(performance-enum-size)
enum class Side : std::uint8_t { kBuy = 0, kSell = 1 };

/// NOLINTNEXTLINE(performance-enum-size)
enum class OrderType : std::uint8_t { kLimit = 0, kMarket = 1 };

/// Closed set of validation-failure reasons (AEGIS-035, ADR-0011). Every
/// enumerator is decided before an order exists — an outcome that depends on
/// book state (an unfilled market residual, an empty book) is a termination,
/// never a rejection (see `TerminationReason` below).
///
/// NOLINTNEXTLINE(performance-enum-size)
enum class RejectReason : std::uint8_t {
  kUnknownInstrument = 0,
  kDuplicateClientOrderId = 1,
  kNonPositiveQuantity = 2,
  kQuantityNotOnLot = 3,
  kQuantityOutOfRange = 4,
  kPriceNotOnTick = 5,
  kPriceOutOfBand = 6,
  kPriceOnMarketOrder = 7,
  kUnknownOrderId = 8,
  kNotOrderOwner = 9,
  kModifyBelowFilled = 10,
  kMalformedMessage = 11,
};

[[nodiscard]] bool is_known_reject_reason(std::uint8_t value);
[[nodiscard]] std::string_view describe(RejectReason reason);

/// Every order's removal has exactly one `kOrderTerminated` event and exactly
/// one reason (ADR-0011).
///
/// NOLINTNEXTLINE(performance-enum-size)
enum class TerminationReason : std::uint8_t {
  kFilled = 0,
  kCanceled = 1,
  kResidualCanceled = 2,
  kReplaced = 3,
};

[[nodiscard]] bool is_known_termination_reason(std::uint8_t value);
[[nodiscard]] std::string_view describe(TerminationReason reason);

// -------------------------------------------------------------------- types
//
// Deliberately plain `std::uint64_t`/`std::int64_t`/`std::uint32_t` here, not
// the strong types of `cpp/exchange/order_book/types.hpp`: this layer may not
// depend on `cpp-exchange-order-book` (configs/architecture_rules.yaml), and a
// wire struct's field is a serialization slot, not a place to enforce
// identifier-space separation — that happens once the payload is decoded into
// the exchange's own strong types.

// -------------------------------------------------------------------- commands

struct NewOrderCommand {
  std::uint32_t instrument_id{0};
  std::uint64_t participant_id{0};
  std::uint64_t client_order_id{0};
  Side side{Side::kBuy};
  OrderType order_type{OrderType::kLimit};
  std::int64_t price_units{0};  ///< Ignored (and must be 0) when order_type is kMarket.
  std::int64_t quantity_units{0};

  friend bool operator==(const NewOrderCommand&, const NewOrderCommand&) = default;
};

struct CancelOrderCommand {
  std::uint32_t instrument_id{0};
  std::uint64_t participant_id{0};
  std::uint64_t order_id{0};

  friend bool operator==(const CancelOrderCommand&, const CancelOrderCommand&) = default;
};

struct ModifyOrderCommand {
  std::uint32_t instrument_id{0};
  std::uint64_t participant_id{0};
  std::uint64_t order_id{0};
  std::int64_t new_price_units{0};
  std::int64_t new_quantity_units{0};

  friend bool operator==(const ModifyOrderCommand&, const ModifyOrderCommand&) = default;
};

// ---------------------------------------------------------------------- events

struct OrderAcceptedEvent {
  std::uint64_t causing_command_sequence{0};
  std::uint64_t order_id{0};
  std::uint32_t instrument_id{0};
  std::uint64_t participant_id{0};
  std::uint64_t client_order_id{0};
  Side side{Side::kBuy};
  OrderType order_type{OrderType::kLimit};
  std::int64_t price_units{0};
  std::int64_t quantity_units{0};  ///< original_quantity at acceptance.

  friend bool operator==(const OrderAcceptedEvent&, const OrderAcceptedEvent&) = default;
};

struct OrderRejectedEvent {
  std::uint64_t causing_command_sequence{0};
  std::uint32_t instrument_id{0};
  std::uint64_t participant_id{0};
  std::uint64_t client_order_id{0};
  RejectReason reason{RejectReason::kMalformedMessage};

  friend bool operator==(const OrderRejectedEvent&, const OrderRejectedEvent&) = default;
};

/// The in-place, priority-retaining quantity decrease (AEGIS-031, ADR-0011).
struct OrderModifiedEvent {
  std::uint64_t causing_command_sequence{0};
  std::uint64_t order_id{0};
  std::int64_t new_remaining_units{0};
  std::int64_t cancelled_quantity_delta_units{0};  ///< Added to cancelled_quantity by this modify.

  friend bool operator==(const OrderModifiedEvent&, const OrderModifiedEvent&) = default;
};

/// Cancel-replace on quantity increase or any price change: `old_order_id`
/// terminates (separately, via `OrderTerminatedEvent{kReplaced}`) and
/// `new_order_id` is accepted at the tail of its level.
struct OrderReplacedEvent {
  std::uint64_t causing_command_sequence{0};
  std::uint64_t old_order_id{0};
  std::uint64_t new_order_id{0};
  std::uint32_t instrument_id{0};
  std::uint64_t participant_id{0};
  std::uint64_t client_order_id{0};
  Side side{Side::kBuy};
  OrderType order_type{OrderType::kLimit};
  std::int64_t price_units{0};
  std::int64_t quantity_units{0};  ///< original_quantity of the replacement.

  friend bool operator==(const OrderReplacedEvent&, const OrderReplacedEvent&) = default;
};

struct TradeEvent {
  std::uint64_t causing_command_sequence{0};
  std::uint32_t instrument_id{0};
  std::int64_t price_units{0};
  std::int64_t quantity_units{0};
  std::uint64_t maker_order_id{0};
  std::uint64_t taker_order_id{0};
  std::uint64_t maker_participant_id{0};
  std::uint64_t taker_participant_id{0};
  Side taker_side{Side::kBuy};

  friend bool operator==(const TradeEvent&, const TradeEvent&) = default;
};

struct OrderTerminatedEvent {
  std::uint64_t causing_command_sequence{0};
  std::uint64_t order_id{0};
  TerminationReason reason{TerminationReason::kFilled};
  std::int64_t cancelled_quantity_delta_units{
      0};  ///< Added to cancelled_quantity by this termination.

  friend bool operator==(const OrderTerminatedEvent&, const OrderTerminatedEvent&) = default;
};

// --------------------------------------------------------------- encode/decode
//
// Each pair encodes/decodes the *payload only* — the caller frames it in an
// `Envelope` (message_type set to the matching `MessageType`, `sequence` set
// to the CommandSequence for a command or the EventSequence for an event).
// Decoding a truncated or malformed payload returns `std::nullopt` rather than
// throwing: a corrupt journal record is an expected failure mode a reader must
// report, not a crash.

[[nodiscard]] std::vector<std::byte> encode(const NewOrderCommand& command);
[[nodiscard]] std::optional<NewOrderCommand> decode_new_order(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode(const CancelOrderCommand& command);
[[nodiscard]] std::optional<CancelOrderCommand> decode_cancel_order(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode(const ModifyOrderCommand& command);
[[nodiscard]] std::optional<ModifyOrderCommand> decode_modify_order(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode(const OrderAcceptedEvent& event);
[[nodiscard]] std::optional<OrderAcceptedEvent> decode_order_accepted(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode(const OrderRejectedEvent& event);
[[nodiscard]] std::optional<OrderRejectedEvent> decode_order_rejected(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode(const OrderModifiedEvent& event);
[[nodiscard]] std::optional<OrderModifiedEvent> decode_order_modified(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode(const OrderReplacedEvent& event);
[[nodiscard]] std::optional<OrderReplacedEvent> decode_order_replaced(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode(const TradeEvent& event);
[[nodiscard]] std::optional<TradeEvent> decode_trade(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode(const OrderTerminatedEvent& event);
[[nodiscard]] std::optional<OrderTerminatedEvent> decode_order_terminated(
    std::span<const std::byte> bytes);

}  // namespace aegis::events::exchange
