#pragma once

#include <compare>
#include <cstdint>

/// The two sequence-number spaces that outlive any single layer (ADR-0012).
///
/// `CommandSequence` and `EventSequence` live in `cpp/events`, not
/// `cpp/exchange/order_book`, because `cpp-exchange-sequencer` may depend on
/// `cpp-common` and `cpp-events` only (`configs/architecture_rules.yaml`) — it
/// assigns `CommandSequence` and derives `ExchangeTime` and must not reach
/// into the order-book layer to do it. Both values are also exactly what gets
/// stamped into `Envelope.sequence`: `CommandSequence` when framing a command
/// for the journal, `EventSequence` when framing an event for the canonical
/// output stream. Distinct, non-implicitly-convertible types, so a bug that
/// swaps one for the other fails to compile.
namespace aegis::events {

namespace detail {
template <typename Tag>
class StrongSequence {
 public:
  constexpr StrongSequence() = default;
  constexpr explicit StrongSequence(std::uint64_t value) : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const { return value_; }

  constexpr auto operator<=>(const StrongSequence&) const = default;
  constexpr bool operator==(const StrongSequence&) const = default;

 private:
  std::uint64_t value_{0};
};
}  // namespace detail

namespace tag {
struct CommandSequenceTag {};
struct EventSequenceTag {};
}  // namespace tag

/// FIFO priority key and causal reference. Assigned by the `Sequencer` on
/// every command *received*, including commands that will be rejected. Starts
/// at 1, `+1` per command, no gaps.
using CommandSequence = detail::StrongSequence<tag::CommandSequenceTag>;

/// The ordering key of the canonical output stream. Assigned by
/// `state::EventLog` on every emitted event — one accept can produce several
/// (accept + trades + terminations). Starts at 1, `+1` per event, strictly
/// monotonic for the whole exchange run.
using EventSequence = detail::StrongSequence<tag::EventSequenceTag>;

}  // namespace aegis::events
