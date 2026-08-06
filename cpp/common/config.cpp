#include "cpp/common/config.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace aegis::common {
namespace {

using nlohmann::json;

/// Keywords this validator implements. Anything else in the schema is reported
/// as unsupported: silently ignoring an unknown keyword means accepting
/// documents the schema author intended to reject, while still looking like
/// validation is happening.
constexpr std::array<std::string_view, 17> kSupportedKeywords = {
    "$schema",   "$id",        "title",    "description",
    "type",      "properties", "required", "additionalProperties",
    "enum",      "minimum",    "maximum",  "minLength",
    "maxLength", "pattern",    "items",    "examples",
    "default"};

bool type_name_matches(const json& value, std::string_view type) {
  if (type == "object") {
    return value.is_object();
  }
  if (type == "array") {
    return value.is_array();
  }
  if (type == "string") {
    return value.is_string();
  }
  if (type == "boolean") {
    return value.is_boolean();
  }
  if (type == "integer") {
    return value.is_number_integer() || value.is_number_unsigned();
  }
  if (type == "number") {
    return value.is_number();
  }
  if (type == "null") {
    return value.is_null();
  }
  return false;
}

/// `type` may be a single name or a list of alternatives, as in the log-record
/// schema's scalar-only `fields` constraint.
bool type_matches(const json& value, const json& type) {
  if (type.is_array()) {
    return std::ranges::any_of(type, [&value](const json& alternative) {
      return type_name_matches(value, alternative.get<std::string>());
    });
  }
  return type_name_matches(value, type.get<std::string>());
}

std::string describe_type(const json& type) {
  if (!type.is_array()) {
    return type.get<std::string>();
  }
  std::string names;
  for (const auto& alternative : type) {
    if (!names.empty()) {
      names.append(" or ");
    }
    names.append(alternative.get<std::string>());
  }
  return names;
}

std::string join(std::string_view path, std::string_view key) {
  if (path.empty()) {
    return std::string{key};
  }
  std::string joined{path};
  joined.append(".").append(key);
  return joined;
}

std::string problem(std::string_view where, std::string_view detail) {
  std::string message{where};
  message.append(": ").append(detail);
  return message;
}

void check_supported_keywords(const json& schema, std::string_view where,
                              std::vector<std::string>& problems) {
  for (const auto& entry : schema.items()) {
    const auto& keyword = entry.key();
    if (std::ranges::find(kSupportedKeywords, keyword) == kSupportedKeywords.end()) {
      problems.push_back(problem(where, "schema uses unsupported keyword '" + keyword +
                                            "'; this validator would otherwise accept documents "
                                            "the schema rejects"));
    }
  }
}

void check_numeric_bounds(const json& value, const json& schema, std::string_view where,
                          std::vector<std::string>& problems) {
  if (!value.is_number()) {
    return;
  }
  if (schema.contains("minimum") && value.get<double>() < schema.at("minimum").get<double>()) {
    problems.push_back(
        problem(where, value.dump() + " is less than the minimum " + schema.at("minimum").dump()));
  }
  if (schema.contains("maximum") && value.get<double>() > schema.at("maximum").get<double>()) {
    problems.push_back(problem(
        where, value.dump() + " is greater than the maximum " + schema.at("maximum").dump()));
  }
}

void check_string_constraints(const json& value, const json& schema, std::string_view where,
                              std::vector<std::string>& problems) {
  if (!value.is_string()) {
    return;
  }
  const auto text = value.get<std::string>();
  if (schema.contains("minLength") && text.size() < schema.at("minLength").get<std::size_t>()) {
    problems.push_back(
        problem(where, "string is shorter than minLength " + schema.at("minLength").dump()));
  }
  if (schema.contains("maxLength") && text.size() > schema.at("maxLength").get<std::size_t>()) {
    problems.push_back(
        problem(where, "string is longer than maxLength " + schema.at("maxLength").dump()));
  }
  if (!schema.contains("pattern")) {
    return;
  }
  const auto pattern = schema.at("pattern").get<std::string>();
  try {
    const std::regex expression(pattern, std::regex::ECMAScript);
    if (!std::regex_search(text, expression)) {
      problems.push_back(problem(where, value.dump() + " does not match pattern " + pattern));
    }
  } catch (const std::regex_error& error) {
    problems.push_back(problem(
        where, "schema pattern is not a valid regular expression: " + std::string{error.what()}));
  }
}

// A schema is a tree and validating one is a tree walk, so the mutual recursion
// between validate_node and check_object is structural rather than incidental.
// Depth is bounded by the schema's own nesting, which is authored in this
// repository and reviewed; this is not an unbounded walk over untrusted input.
// NOLINTNEXTLINE(misc-no-recursion)
void validate_node(const json& value, const json& schema, const std::string& path,
                   std::vector<std::string>& problems);

// NOLINTNEXTLINE(misc-no-recursion)
void check_object(const json& value, const json& schema, const std::string& path,
                  std::vector<std::string>& problems) {
  if (schema.contains("required")) {
    for (const auto& name : schema.at("required")) {
      const auto key = name.get<std::string>();
      if (!value.contains(key)) {
        problems.push_back(problem(join(path, key), "required field is missing"));
      }
    }
  }

  // additionalProperties is either a boolean or a schema every unlisted property
  // must satisfy. Both forms appear in the AEGIS schemas.
  const bool has_additional = schema.contains("additionalProperties");
  const json additional = has_additional ? schema.at("additionalProperties") : json();
  const bool additional_allowed =
      !has_additional || !additional.is_boolean() || additional.get<bool>();
  const json empty_properties = json::object();
  const json& properties =
      schema.contains("properties") ? schema.at("properties") : empty_properties;

  for (const auto& entry : value.items()) {
    const auto& key = entry.key();
    if (properties.contains(key)) {
      validate_node(entry.value(), properties.at(key), join(path, key), problems);
    } else if (!additional_allowed) {
      problems.push_back(problem(join(path, key),
                                 "unknown field; a typo here would otherwise be silently ignored"));
    } else if (has_additional && additional.is_object()) {
      validate_node(entry.value(), additional, join(path, key), problems);
    }
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
void validate_node(const json& value, const json& schema, const std::string& path,
                   std::vector<std::string>& problems) {
  const std::string where = path.empty() ? "(root)" : path;

  check_supported_keywords(schema, where, problems);

  if (schema.contains("type")) {
    const auto& expected = schema.at("type");
    if (!type_matches(value, expected)) {
      problems.push_back(problem(where, "expected " + describe_type(expected) + ", got " +
                                            std::string{value.type_name()}));
      return;  // further checks would only report noise derived from the wrong type
    }
  }

  if (schema.contains("enum")) {
    const auto& allowed = schema.at("enum");
    if (std::ranges::find(allowed, value) == allowed.end()) {
      problems.push_back(problem(where, value.dump() + " is not one of " + allowed.dump()));
    }
  }

  check_numeric_bounds(value, schema, where, problems);
  check_string_constraints(value, schema, where, problems);

  if (value.is_object()) {
    check_object(value, schema, path, problems);
  }
}

/// Read a whole file, returning an empty string when it cannot be opened.
///
/// Callers distinguish "missing" from "empty" by context: neither a schema nor a
/// configuration document may legitimately be empty, so one signal suffices.
std::string read_file(const std::filesystem::path& path) {
  const std::ifstream input{path};
  if (!input) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

}  // namespace

const nlohmann::json* Config::find(std::string_view dotted) const {
  const json* node = &values_;
  std::string segment;
  std::istringstream stream{std::string{dotted}};
  while (std::getline(stream, segment, '.')) {
    if (!node->is_object() || !node->contains(segment)) {
      return nullptr;
    }
    node = &node->at(segment);
  }
  return node;
}

std::vector<std::string> validate(const nlohmann::json& document, const nlohmann::json& schema) {
  std::vector<std::string> problems;

  // config_version is checked before anything else. A document written for a
  // different schema must be rejected, not reinterpreted under today's rules:
  // reinterpretation silently changes what the run does.
  if (!document.contains("config_version")) {
    problems.emplace_back(
        "config_version: required field is missing. AEGIS refuses to guess which schema a "
        "configuration targets.");
  } else if (!document.at("config_version").is_number_integer() ||
             document.at("config_version").get<int>() != kConfigSchemaVersion) {
    problems.push_back("config_version: " + document.at("config_version").dump() +
                       " is not supported by this build (this build understands version " +
                       std::to_string(kConfigSchemaVersion) + ")");
  }

  validate_node(document, schema, "", problems);
  return problems;
}

Config load_config_from_string(std::string_view text, const nlohmann::json& schema,
                               std::string_view origin) {
  json document;
  try {
    document = json::parse(text);
  } catch (const json::parse_error& error) {
    throw ConfigError(std::string{origin} + ": not valid JSON: " + error.what());
  }
  if (!document.is_object()) {
    throw ConfigError(std::string{origin} + ": configuration must be a mapping, got " +
                      std::string{document.type_name()});
  }

  const auto problems = validate(document, schema);
  if (!problems.empty()) {
    std::ostringstream message;
    message << origin << ": configuration is invalid (" << problems.size() << " problem(s)):";
    for (const auto& entry : problems) {
      message << "\n  - " << entry;
    }
    throw ConfigError(message.str());
  }
  return Config{std::move(document)};
}

nlohmann::json load_schema(const std::filesystem::path& schema_path) {
  const auto text = read_file(schema_path);
  if (text.empty()) {
    throw ConfigError("configuration schema not found at " + schema_path.string());
  }
  try {
    return json::parse(text);
  } catch (const json::parse_error& error) {
    throw ConfigError("configuration schema " + schema_path.string() +
                      " is not valid JSON: " + std::string{error.what()});
  }
}

Config load_config(const std::filesystem::path& config_path,
                   const std::filesystem::path& schema_path) {
  const auto text = read_file(config_path);
  if (text.empty()) {
    throw ConfigError("configuration file not found: " + config_path.string());
  }
  return load_config_from_string(text, load_schema(schema_path), config_path.string());
}

}  // namespace aegis::common
