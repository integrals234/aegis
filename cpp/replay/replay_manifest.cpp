#include "cpp/replay/replay_manifest.hpp"

#include <sstream>

#include <nlohmann/json.hpp>

namespace aegis::replay {

namespace {
using Json = nlohmann::json;

/// One record's canonical text form for the digest: every field that
/// participates in canonical order, delimited so no field's own content can
/// be mistaken for a delimiter under this milestone's identifier grammar
/// (`python/futures/identifiers.py`: contract symbols are `[A-Z0-9:]` only).
std::string canonical_text(const ReplayEvent& event) {
  std::ostringstream out;
  out << event.event_time.nanos() << '|' << event.source_sequence.value() << '|'
      << event.contract_symbol << '|' << event.record_index.value() << '\n';
  return out.str();
}

}  // namespace

std::uint64_t fnv1a64(std::string_view data) {
  // FNV-1a 64-bit: the standard offset basis and prime. Deterministic and
  // dependency-free -- exactly what a reproducibility digest needs; not a
  // cryptographic claim.
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;
  for (const char character : data) {
    hash ^= static_cast<unsigned char>(character);
    hash *= kFnvPrime;
  }
  return hash;
}

ReplayManifest compute_manifest(const std::string& input_path,
                                const std::vector<ReplayEvent>& events) {
  ReplayManifest manifest;
  manifest.input_path = input_path;
  manifest.record_count = events.size();
  if (!events.empty()) {
    manifest.first_event_time_nanos = events.front().event_time.nanos();
    manifest.last_event_time_nanos = events.back().event_time.nanos();
  }

  std::string canonical_bytes;
  for (const auto& event : events) {
    canonical_bytes += canonical_text(event);
  }
  manifest.content_digest = fnv1a64(canonical_bytes);
  return manifest;
}

std::string to_json(const ReplayManifest& manifest) {
  const Json document{
      {"input_path", manifest.input_path},
      {"record_count", manifest.record_count},
      {"first_event_time_nanos", manifest.first_event_time_nanos},
      {"last_event_time_nanos", manifest.last_event_time_nanos},
      {"content_digest", manifest.content_digest},
  };
  return document.dump();
}

}  // namespace aegis::replay
