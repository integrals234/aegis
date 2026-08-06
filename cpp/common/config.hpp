#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

/// Versioned, validated configuration for the C++ side (AEGIS-231).
///
/// This loader validates against the *same* file the Python reference loader
/// uses — `configs/schemas/config.v1.json`. Two loaders with two private copies
/// of the rules diverge, and the divergence surfaces as a run that behaved
/// differently from the one its configuration describes. So the schema is read
/// at runtime rather than transcribed into C++.
///
/// The validator implements the subset of JSON Schema the AEGIS schema uses:
/// type, required, additionalProperties, enum, minimum/maximum,
/// minLength/maxLength and pattern. Anything outside that subset is reported as
/// an unsupported keyword rather than quietly ignored — a validator that skips
/// what it does not understand accepts documents it was meant to reject.
namespace aegis::common {

/// Thrown when a configuration is rejected. The message names every problem.
class ConfigError : public std::runtime_error {
 public:
  explicit ConfigError(const std::string& message) : std::runtime_error(message) {}
};

inline constexpr int kConfigSchemaVersion = 1;

/// A validated configuration document.
class Config {
 public:
  explicit Config(nlohmann::json values) : values_(std::move(values)) {}

  [[nodiscard]] const nlohmann::json& values() const { return values_; }

  [[nodiscard]] int config_version() const { return values_.at("config_version").get<int>(); }

  [[nodiscard]] std::string experiment_id() const {
    return values_.at("run").at("experiment_id").get<std::string>();
  }

  /// Look up a dotted path, returning nullptr when any segment is absent.
  [[nodiscard]] const nlohmann::json* find(std::string_view dotted) const;

  /// Deterministic serialization: sorted keys, no insignificant whitespace.
  ///
  /// nlohmann::json orders object keys lexicographically by default, which is
  /// what makes this stable across runs and across the Python peer.
  [[nodiscard]] std::string canonical_json() const { return values_.dump(); }

 private:
  nlohmann::json values_;
};

/// Validate a document against a schema, returning every problem found.
///
/// Returns rather than throws, and collects rather than stops at the first
/// failure: fixing a configuration one error per run is a poor use of anyone's
/// afternoon.
[[nodiscard]] std::vector<std::string> validate(const nlohmann::json& document,
                                                const nlohmann::json& schema);

/// Load and validate a configuration document held in memory.
///
/// `origin` appears in error messages so a failure names the file it came from.
[[nodiscard]] Config load_config_from_string(std::string_view text, const nlohmann::json& schema,
                                             std::string_view origin = "<string>");

/// Load and validate a configuration file against a schema file.
[[nodiscard]] Config load_config(const std::filesystem::path& config_path,
                                 const std::filesystem::path& schema_path);

/// Read a schema file, failing loudly if it is missing or malformed.
[[nodiscard]] nlohmann::json load_schema(const std::filesystem::path& schema_path);

}  // namespace aegis::common
