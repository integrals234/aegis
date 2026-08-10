#include "cpp/replay/fault_injection.hpp"

#include <stdexcept>
#include <unordered_map>

namespace aegis::replay {

std::string describe(FaultKind kind) {
  switch (kind) {
    case FaultKind::kDelayed:
      return "delayed";
    case FaultKind::kMissing:
      return "missing";
    case FaultKind::kDuplicated:
      return "duplicated";
    case FaultKind::kSequenceGap:
      return "sequence_gap";
  }
  return "unknown_fault_kind";  // pragma: exhaustive enum above
}

FaultInjectionResult DeterministicFaultInjector::apply(const std::vector<ReplayEvent>& input,
                                                       const std::vector<FaultRule>& rules) {
  // Keyed by record_index for O(1) lookup only -- iteration always walks
  // `input` in its own already-canonical order, so this map's hash order
  // never influences the result.
  std::unordered_map<std::uint64_t, FaultRule> rule_by_target;
  for (const auto& rule : rules) {
    const auto key = rule.target.value();
    if (rule_by_target.contains(key)) {
      throw std::invalid_argument(
          "DeterministicFaultInjector::apply: multiple rules target the same record_index (" +
          std::to_string(key) + ")");
    }
    rule_by_target.emplace(key, rule);
  }

  FaultInjectionResult result;
  for (const auto& event : input) {
    const auto found = rule_by_target.find(event.record_index.value());
    if (found == rule_by_target.end()) {
      result.events.emplace_back(event, std::nullopt);
      continue;
    }

    const auto& rule = found->second;
    switch (rule.kind) {
      case FaultKind::kMissing:
        result.dropped.push_back(
            DroppedRecord{.record_index = event.record_index, .reason = FaultKind::kMissing});
        break;
      case FaultKind::kDuplicated:
        result.events.emplace_back(event, std::nullopt);
        result.events.emplace_back(
            event, FaultAnnotation{.kind = FaultKind::kDuplicated, .delay = {}, .magnitude = 0});
        break;
      case FaultKind::kDelayed:
        result.events.emplace_back(
            event,
            FaultAnnotation{.kind = FaultKind::kDelayed, .delay = rule.delay, .magnitude = 0});
        break;
      case FaultKind::kSequenceGap:
        result.events.emplace_back(
            event, FaultAnnotation{
                       .kind = FaultKind::kSequenceGap, .delay = {}, .magnitude = rule.magnitude});
        break;
    }
  }
  return result;
}

}  // namespace aegis::replay
