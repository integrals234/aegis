#include "cpp/replay/replay_event.hpp"

#include <compare>

namespace aegis::replay {

std::strong_ordering canonical_compare(const ReplayEvent& lhs, const ReplayEvent& rhs) {
  // Written as explicit early returns rather than a tuple comparison so the
  // precedence is readable at the point of maintenance: this order IS the
  // determinism guarantee, and a reader must be able to check it without
  // reconstructing what std::tie does.
  if (const auto order = lhs.event_time <=> rhs.event_time; order != std::strong_ordering::equal) {
    return order;
  }
  if (const auto order = lhs.source_sequence <=> rhs.source_sequence;
      order != std::strong_ordering::equal) {
    return order;
  }
  // Byte-wise, not locale-aware. A locale-sensitive collation would make the
  // canonical order depend on the machine's environment, which is exactly the
  // class of dependency deterministic replay exists to exclude.
  if (const auto order = lhs.contract_symbol.compare(rhs.contract_symbol); order != 0) {
    return order < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
  }
  // Unique by construction across a canonical ingestion result, so this is the
  // component that makes the key total rather than merely partial.
  return lhs.record_index <=> rhs.record_index;
}

bool canonical_less(const ReplayEvent& lhs, const ReplayEvent& rhs) {
  return canonical_compare(lhs, rhs) == std::strong_ordering::less;
}

}  // namespace aegis::replay
