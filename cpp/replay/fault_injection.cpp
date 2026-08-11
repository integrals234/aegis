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
    case FaultKind::kSpreadWidening:
      return "spread_widening";
    case FaultKind::kVolatilitySpike:
      return "volatility_spike";
    case FaultKind::kLiquidityVanish:
      return "liquidity_vanish";
    case FaultKind::kRejection:
      return "rejection";
    case FaultKind::kLatencySpike:
      return "latency_spike";
    case FaultKind::kPartialFill:
      return "partial_fill";
    case FaultKind::kBackpressure:
      return "backpressure";
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
        result.events.emplace_back(event, FaultAnnotation{.kind = FaultKind::kDuplicated,
                                                          .delay = rule.delay,
                                                          .magnitude = rule.magnitude});
        break;
      // Every other kind is a pure annotation: the record survives,
      // untouched, with the rule's own parameters attached verbatim. This
      // covers AEGIS-060/061's kDelayed/kSequenceGap and AEGIS-062/063's
      // seven market/execution stress kinds identically -- none of them
      // interprets its own parameters, they are just carried through for a
      // later milestone's consumer to read.
      case FaultKind::kDelayed:
      case FaultKind::kSequenceGap:
      case FaultKind::kSpreadWidening:
      case FaultKind::kVolatilitySpike:
      case FaultKind::kLiquidityVanish:
      case FaultKind::kRejection:
      case FaultKind::kLatencySpike:
      case FaultKind::kPartialFill:
      case FaultKind::kBackpressure:
        result.events.emplace_back(
            event,
            FaultAnnotation{.kind = rule.kind, .delay = rule.delay, .magnitude = rule.magnitude});
        break;
    }
  }
  return result;
}

}  // namespace aegis::replay
