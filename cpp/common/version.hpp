#pragma once

#include <string>

namespace aegis::common {

/// Semantic version of the AEGIS platform, as declared by the CMake project.
[[nodiscard]] std::string version();

/// Compiler, standard, build type and sanitizer configuration of this binary.
///
/// Recorded rather than described: docs/BENCHMARK_POLICY.md requires every
/// performance claim to disclose the toolchain that produced it, and a build
/// that cannot report its own configuration cannot support such a claim
/// (AEGIS-053, AEGIS-009).
[[nodiscard]] std::string build_info();

/// True when this binary was built with assertions enabled.
///
/// Latency measured from an assertion-enabled build is not a release figure,
/// so the distinction has to be observable at runtime, not remembered.
[[nodiscard]] bool assertions_enabled();

/// True when this binary was built with a sanitizer active.
[[nodiscard]] bool sanitizers_enabled();

}  // namespace aegis::common
