#pragma once

#include <compare>
#include <cstdint>

#include "cpp/events/exchange_messages.hpp"
#include "cpp/events/sequence.hpp"

/// Exchange order-book identifier spaces and price/quantity units (ADR-0009,
/// ADR-0012).
///
/// Three identifier spaces — `CommandSequence`, `EventSequence`, `OrderId` —
/// are distinct strong types with no implicit conversion between them or to a
/// raw integer, so a bug that swaps one for another fails to compile instead
/// of silently corrupting priority, event order, or order identity.
/// `CommandSequence`/`EventSequence` are not defined here: `cpp-exchange-sequencer`
/// may depend on `cpp-common` and `cpp-events` only, never on this layer
/// (`configs/architecture_rules.yaml`), so both live in `cpp/events/sequence.hpp`
/// and are aliased here. `Side`, `OrderType`, `RejectReason` and
/// `TerminationReason` are aliased the same way from `cpp/events/exchange_messages.hpp`
/// (ADR-0009) — one definition, reused by every layer permitted to see it.
namespace aegis::exchange {

using Side = events::exchange::Side;
using OrderType = events::exchange::OrderType;
using RejectReason = events::exchange::RejectReason;
using TerminationReason = events::exchange::TerminationReason;
using CommandSequence = events::CommandSequence;
using EventSequence = events::EventSequence;

namespace detail {

/// A bare identifier: equality and ordering only, no arithmetic. Ordering
/// exists so these can key a `std::map` or sort deterministically in
/// canonical output; addition or subtraction on an id would be a bug the type
/// system should catch, not a valid operation.
template <typename Tag, typename Repr>
class StrongId {
 public:
  constexpr StrongId() = default;
  constexpr explicit StrongId(Repr value) : value_(value) {}

  [[nodiscard]] constexpr Repr value() const { return value_; }

  constexpr auto operator<=>(const StrongId&) const = default;
  constexpr bool operator==(const StrongId&) const = default;

 private:
  Repr value_{};
};

}  // namespace detail

namespace tag {
struct OrderIdTag {};
struct InstrumentIdTag {};
struct ParticipantIdTag {};
struct ClientOrderIdTag {};
}  // namespace tag

/// Assigned by `MatchingEngine` on acceptance only. A rejected `NewOrder`
/// consumes none; a cancel-replace allocates a new one.
using OrderId = detail::StrongId<tag::OrderIdTag, std::uint64_t>;

using InstrumentId = detail::StrongId<tag::InstrumentIdTag, std::uint32_t>;
using ParticipantId = detail::StrongId<tag::ParticipantIdTag, std::uint64_t>;

/// Scoped to `(ParticipantId, ClientOrderId)`, not globally unique — two
/// participants may reuse the same value (ADR-0011).
using ClientOrderId = detail::StrongId<tag::ClientOrderIdTag, std::uint64_t>;

/// An order's queue position, distinct from the `CommandSequence` that
/// produced it only in name — the type exists so "this is a priority" and
/// "this is a raw sequence number" cannot be confused at a call site.
/// Constructible only via `Priority::from`.
class Priority {
 public:
  constexpr Priority() = default;

  [[nodiscard]] static constexpr Priority from(CommandSequence sequence) {
    return Priority{sequence};
  }

  [[nodiscard]] constexpr CommandSequence command_sequence() const { return sequence_; }

  constexpr auto operator<=>(const Priority&) const = default;
  constexpr bool operator==(const Priority&) const = default;

 private:
  constexpr explicit Priority(CommandSequence sequence) : sequence_(sequence) {}
  CommandSequence sequence_;
};

/// The smallest representable price unit; the instrument declares the grid on
/// top of it (`InstrumentSpec`). Arithmetic is intentionally available —
/// levels are keyed on it and matching compares it — unlike the bare
/// identifiers above.
class PriceUnits {
 public:
  constexpr PriceUnits() = default;
  constexpr explicit PriceUnits(std::int64_t value) : value_(value) {}

  [[nodiscard]] constexpr std::int64_t value() const { return value_; }

  constexpr auto operator<=>(const PriceUnits&) const = default;
  constexpr bool operator==(const PriceUnits&) const = default;

 private:
  std::int64_t value_{0};
};

/// The smallest representable quantity unit; `lot_size_units` declares the
/// grid. Arithmetic is available for fills, residuals and level aggregates.
class QuantityUnits {
 public:
  constexpr QuantityUnits() = default;
  constexpr explicit QuantityUnits(std::int64_t value) : value_(value) {}

  [[nodiscard]] constexpr std::int64_t value() const { return value_; }

  constexpr auto operator<=>(const QuantityUnits&) const = default;
  constexpr bool operator==(const QuantityUnits&) const = default;

  constexpr QuantityUnits operator+(QuantityUnits other) const {
    return QuantityUnits{value_ + other.value_};
  }
  constexpr QuantityUnits operator-(QuantityUnits other) const {
    return QuantityUnits{value_ - other.value_};
  }
  constexpr QuantityUnits& operator+=(QuantityUnits other) {
    value_ += other.value_;
    return *this;
  }
  constexpr QuantityUnits& operator-=(QuantityUnits other) {
    value_ -= other.value_;
    return *this;
  }

 private:
  std::int64_t value_{0};
};

[[nodiscard]] constexpr QuantityUnits min(QuantityUnits a, QuantityUnits b) {
  return a < b ? a : b;
}

inline constexpr QuantityUnits kZeroQuantity{0};

}  // namespace aegis::exchange
