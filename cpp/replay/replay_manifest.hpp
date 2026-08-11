#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/replay/replay_event.hpp"

/// A deterministic manifest describing one loaded replay stream (M2 slice 9,
/// AEGIS-058).
///
/// The manifest exists to make "repeated runs produce identical outputs"
/// (the frozen acceptance) independently checkable without diffing the
/// entire emitted stream: two runs over the same input produce the same
/// manifest, and a manifest mismatch is cheaper to report than a full
/// output diff. `content_digest` is FNV-1a, 64-bit -- deterministic and
/// dependency-free, which is all a reproducibility check needs; it is not a
/// cryptographic integrity claim, and nothing in this milestone asks it to
/// be one, so no new hashing dependency was added for it.
namespace aegis::replay {

struct ReplayManifest {
  std::string input_path;
  std::uint64_t record_count{0};
  std::int64_t first_event_time_nanos{0};
  std::int64_t last_event_time_nanos{0};
  std::uint64_t content_digest{0};

  friend bool operator==(const ReplayManifest&, const ReplayManifest&) = default;
};

/// FNV-1a, 64-bit. Exposed for the property test that checks it against an
/// independent reference implementation, not only through the manifest.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view data);

/// `events` must already be canonically ordered (as `load_replay_stream`
/// guarantees) -- the digest is computed over that exact order, so it is
/// sensitive to both content and sequence.
[[nodiscard]] ReplayManifest compute_manifest(const std::string& input_path,
                                              const std::vector<ReplayEvent>& events);

/// Canonical JSON form, sorted keys, no insignificant whitespace -- the same
/// discipline `python/common/determinism.py`'s canonical document uses, so a
/// manifest is diffable and hashable on its own.
[[nodiscard]] std::string to_json(const ReplayManifest& manifest);

}  // namespace aegis::replay
