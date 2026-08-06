#include "cpp/common/logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "cpp/common/clock.hpp"

namespace aegis::common {
namespace {

using nlohmann::json;

/// Substrings that mark a field as holding a credential. Matched against the
/// lowercased key, so `API_KEY`, `db.password` and `awsSecret` all hit.
constexpr std::string_view kRedacted = "[redacted]";

constexpr std::array<std::string_view, 9> kSecretMarkers = {
    "password",   "passwd",  "secret",        "token", "key",
    "credential", "private", "authorization", "auth"};

std::string lowercase(std::string_view text) {
  std::string lowered{text};
  std::ranges::transform(lowered, lowered.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lowered;
}

json to_json_value(const FieldValue& value) {
  return std::visit(
      [](const auto& held) -> json {
        using Held = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<Held, std::monostate>) {
          return nullptr;
        } else {
          return held;
        }
      },
      value);
}

}  // namespace

std::string_view to_string(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return "trace";
    case LogLevel::kDebug:
      return "debug";
    case LogLevel::kInfo:
      return "info";
    case LogLevel::kWarn:
      return "warn";
    case LogLevel::kError:
      return "error";
  }
  return "info";
}

std::optional<LogLevel> parse_log_level(std::string_view name) {
  const auto lowered = lowercase(name);
  if (lowered == "trace") return LogLevel::kTrace;
  if (lowered == "debug") return LogLevel::kDebug;
  if (lowered == "info") return LogLevel::kInfo;
  if (lowered == "warn") return LogLevel::kWarn;
  if (lowered == "error") return LogLevel::kError;
  return std::nullopt;
}

bool is_secret_field(std::string_view name) {
  const auto lowered = lowercase(name);
  return std::ranges::any_of(kSecretMarkers, [&lowered](std::string_view marker) {
    return lowered.find(marker) != std::string::npos;
  });
}

void FileLogSink::write_line(const std::string& line) {
  std::ofstream output{path_, std::ios::app};
  if (!output) {
    throw std::runtime_error("cannot open log sink for append: " + path_);
  }
  output << line << '\n';
}

StructuredLogger::StructuredLogger(std::string name, std::string experiment_id,
                                   const WallClock& clock, LogSink& sink, LogLevel level,
                                   std::string correlation_id)
    : name_(std::move(name)),
      experiment_id_(std::move(experiment_id)),
      clock_(&clock),
      sink_(&sink),
      level_(level),
      correlation_id_(std::move(correlation_id)) {
  if (name_.empty()) {
    throw std::invalid_argument("logger name is required; records must say what emitted them");
  }
  if (experiment_id_.empty()) {
    throw std::invalid_argument(
        "experiment_id is required; a record that cannot be joined to its run is not evidence "
        "of anything");
  }
}

StructuredLogger StructuredLogger::bind(std::string name, std::string correlation_id) const {
  return StructuredLogger{std::move(name), experiment_id_, *clock_,
                          *sink_,          level_,         std::move(correlation_id)};
}

void StructuredLogger::log(LogLevel level, std::string_view message, const Fields& fields) {
  if (static_cast<std::uint8_t>(level) < static_cast<std::uint8_t>(level_)) {
    return;
  }

  // nlohmann's ordered_json preserves insertion order, so the key order below is
  // the key order on disk. Two identical runs therefore produce identical bytes,
  // which is what makes a log file usable as determinism-harness input.
  auto record = nlohmann::ordered_json::object();
  record.emplace("schema_version", kLogSchemaVersion);
  record.emplace("timestamp_ns", clock_->now_utc());
  record.emplace("level", std::string{to_string(level)});
  record.emplace("logger", name_);
  record.emplace("message", std::string{message});
  record.emplace("experiment_id", experiment_id_);
  record.emplace("sequence", sequence_);
  if (!correlation_id_.empty()) {
    record.emplace("correlation_id", correlation_id_);
  }
  if (!fields.empty()) {
    auto payload = nlohmann::ordered_json::object();
    for (const auto& [key, value] : fields) {  // std::map iterates in key order
      payload.emplace(key, is_secret_field(key) ? json(kRedacted) : to_json_value(value));
    }
    record.emplace("fields", std::move(payload));
  }

  ++sequence_;
  sink_->write_line(record.dump());
}

}  // namespace aegis::common
