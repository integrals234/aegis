#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "cpp/common/time.hpp"
#include "cpp/replay/fault_injection.hpp"
#include "cpp/replay/pacing.hpp"
#include "cpp/replay/replay_engine.hpp"
#include "cpp/replay/replay_event.hpp"
#include "cpp/replay/replay_manifest.hpp"
#include "cpp/replay/replay_stream.hpp"
#include "cpp/replay/virtual_clock.hpp"

/// `aegis_replay_run`: the cpp-replay layer's CLI entry point (M2 slice 14,
/// AEGIS-054..058).
///
/// Mirrors what `aegis_exchange_replay` is for the exchange core (ADR-0012):
/// read a canonical JSON-Lines stream, drive it through the real
/// `ReplayEngine`/`VirtualClock`/pacing/fault machinery, and write canonical
/// output lines to stdout so two independent *processes* can be compared byte
/// for byte by `tools/determinism_check.py`.
///
/// Why it exists: before slice 14, M2's replay determinism was demonstrated
/// only in-process (two calls inside one gtest binary). This repository's own
/// AEGIS-005 harness documents why that is not sufficient -- running twice in
/// one process shares process state and "hides exactly the class of
/// nondeterminism that later bites". AEGIS-058's frozen acceptance is
/// "repeated runs produce identical outputs"; this binary is what makes
/// "runs" mean separate OS processes.
///
/// `--label` exists because `ReplayManifest::input_path` would otherwise put a
/// caller-specific (often temporary) path into the output, making two runs over
/// the same *content* differ for a reason that has nothing to do with replay.
/// The label is the stream's logical identity; the bytes it came from are what
/// `content_digest` covers.
namespace {

using aegis::common::Duration;
using aegis::replay::AcceleratedPacing;
using aegis::replay::compute_manifest;
using aegis::replay::DeterministicFaultInjector;
using aegis::replay::FaultAnnotation;
using aegis::replay::FaultKind;
using aegis::replay::FaultRule;
using aegis::replay::FixedRatePacing;
using aegis::replay::load_replay_stream;
using aegis::replay::OriginalSpeedPacing;
using aegis::replay::PacingPolicy;
using aegis::replay::RecordIndex;
using aegis::replay::ReplayEngine;
using aegis::replay::ReplayEvent;
using aegis::replay::StepPacing;
using aegis::replay::VirtualClock;
using Json = nlohmann::json;

struct Options {
  std::string input_path;
  std::optional<std::string> label;
  std::string pacing{"original"};
  double multiplier{1.0};
  std::int64_t interval_nanos{0};
  std::optional<std::uint64_t> resume_after;
  std::vector<FaultRule> faults;
};

/// The CLI spelling of every FaultKind -- one table to extend when the enum
/// grows, exactly as it did in slice 12. Function-local rather than
/// namespace-scope: a namespace-scope container is initialized before main and
/// its constructor can throw where nothing can catch it
/// (bugprone-throwing-static-initialization).
std::optional<FaultKind> parse_fault_kind(const std::string& text) {
  static const std::unordered_map<std::string, FaultKind> fault_kinds{
      {"delayed", FaultKind::kDelayed},
      {"missing", FaultKind::kMissing},
      {"duplicated", FaultKind::kDuplicated},
      {"sequence_gap", FaultKind::kSequenceGap},
      {"spread_widening", FaultKind::kSpreadWidening},
      {"volatility_spike", FaultKind::kVolatilitySpike},
      {"liquidity_vanish", FaultKind::kLiquidityVanish},
      {"rejection", FaultKind::kRejection},
      {"latency_spike", FaultKind::kLatencySpike},
      {"partial_fill", FaultKind::kPartialFill},
      {"backpressure", FaultKind::kBackpressure},
  };
  const auto found = fault_kinds.find(text);
  if (found == fault_kinds.end()) {
    return std::nullopt;
  }
  return found->second;
}

/// `--fault INDEX:KIND[:DELAY_NS[:MAGNITUDE]]`. Explicit, caller-supplied
/// data -- never sampled (ADR-0019).
bool parse_fault(const std::string& spec, FaultRule& rule) {
  std::vector<std::string> parts;
  std::string current;
  for (const char character : spec) {
    if (character == ':') {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(character);
  }
  parts.push_back(current);
  if (parts.size() < 2 || parts.size() > 4) {
    return false;
  }
  const auto kind = parse_fault_kind(parts[1]);
  if (!kind.has_value()) {
    return false;
  }
  try {
    rule.target = RecordIndex{std::stoull(parts[0])};
    rule.kind = *kind;
    rule.delay = Duration{parts.size() > 2 ? std::stoll(parts[2]) : 0};
    rule.magnitude = parts.size() > 3 ? std::stoull(parts[3]) : 0;
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

bool parse_args(int argc, char** argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const bool has_value = (i + 1) < argc;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) - argv is a C array
    const std::string value = has_value ? argv[i + 1] : std::string{};
    if (flag == "--input" && has_value) {
      options.input_path = value;
      ++i;
    } else if (flag == "--label" && has_value) {
      options.label = value;
      ++i;
    } else if (flag == "--pacing" && has_value) {
      options.pacing = value;
      ++i;
    } else if (flag == "--multiplier" && has_value) {
      options.multiplier = std::stod(value);
      ++i;
    } else if (flag == "--interval-ns" && has_value) {
      options.interval_nanos = std::stoll(value);
      ++i;
    } else if (flag == "--resume-after" && has_value) {
      options.resume_after = std::stoull(value);
      ++i;
    } else if (flag == "--fault" && has_value) {
      FaultRule rule;
      if (!parse_fault(value, rule)) {
        error = "malformed --fault specification: " + value;
        return false;
      }
      options.faults.push_back(rule);
      ++i;
    } else {
      error = "unrecognized or incomplete argument: " + flag;
      return false;
    }
  }
  if (options.input_path.empty()) {
    error = "--input is required";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options options;
    std::string error;
    if (!parse_args(argc, argv, options, error)) {
      std::cerr << "aegis_replay_run: " << error << '\n';
      return 2;
    }

    const auto loaded = load_replay_stream(options.input_path);
    if (!loaded.has_value()) {
      std::cerr << "aegis_replay_run: " << aegis::replay::describe(loaded.error()) << ": "
                << loaded.detail() << '\n';
      return 1;
    }

    // The manifest describes the *input* stream, before any fault perturbs it.
    const auto manifest =
        compute_manifest(options.label.value_or(options.input_path), loaded.value());

    // Faults are applied to the validated stream; the engine then drives the
    // (possibly annotated, duplicated or shortened) result. No fault mutates a
    // canonical-order field, so the engine still sees one legal order.
    const auto faulted = DeterministicFaultInjector::apply(loaded.value(), options.faults);
    std::vector<ReplayEvent> events;
    std::vector<std::optional<FaultAnnotation>> annotations;
    events.reserve(faulted.events.size());
    annotations.reserve(faulted.events.size());
    for (const auto& [event, annotation] : faulted.events) {
      events.push_back(event);
      annotations.push_back(annotation);
    }

    std::unique_ptr<PacingPolicy> pacing;
    if (options.pacing == "original") {
      pacing = std::make_unique<OriginalSpeedPacing>();
    } else if (options.pacing == "accelerated") {
      pacing = std::make_unique<AcceleratedPacing>(options.multiplier);
    } else if (options.pacing == "fixed") {
      pacing = std::make_unique<FixedRatePacing>(Duration{options.interval_nanos});
    } else if (options.pacing == "step") {
      pacing = std::make_unique<StepPacing>();
    } else {
      std::cerr << "aegis_replay_run: unknown pacing mode: " << options.pacing << '\n';
      return 2;
    }

    VirtualClock clock;
    ReplayEngine engine(events, clock);
    std::size_t position = 0;
    if (options.resume_after.has_value()) {
      engine.resume_from(RecordIndex{*options.resume_after});
      // resume_from seeks past the named record; mirror that in the parallel
      // annotation index so the two never drift.
      while (position < events.size() &&
             events[position].record_index.value() != *options.resume_after) {
        ++position;
      }
      if (position < events.size()) {
        ++position;
      }
    }

    const Json manifest_line{
        {"type", "manifest"},
        {"input_label", manifest.input_path},
        {"record_count", manifest.record_count},
        {"first_event_time_nanos", manifest.first_event_time_nanos},
        {"last_event_time_nanos", manifest.last_event_time_nanos},
        {"content_digest", manifest.content_digest},
        {"pacing", options.pacing},
        {"faults_applied", options.faults.size()},
        {"records_dropped", faulted.dropped.size()},
    };
    std::cout << manifest_line.dump() << '\n';

    while (const auto emitted = engine.next_with_pacing(*pacing)) {
      const auto& [event, wait] = *emitted;
      const auto& annotation =
          position < annotations.size() ? annotations[position] : std::optional<FaultAnnotation>{};
      const Json line{
          {"type", "event"},
          {"event_time_ns", event.event_time.nanos()},
          {"source_sequence", event.source_sequence.value()},
          {"contract_symbol", event.contract_symbol},
          {"record_index", event.record_index.value()},
          {"wait_nanos", wait.nanos()},
          {"clock_now_ns", clock.now_utc()},
          {"fault", annotation.has_value() ? aegis::replay::describe(annotation->kind) : "none"},
      };
      std::cout << line.dump() << '\n';
      ++position;
    }

    for (const auto& dropped : faulted.dropped) {
      const Json line{
          {"type", "dropped"},
          {"record_index", dropped.record_index.value()},
          {"reason", aegis::replay::describe(dropped.reason)},
      };
      std::cout << line.dump() << '\n';
    }
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "aegis_replay_run: " << exception.what() << '\n';
    return 1;
  }
}
