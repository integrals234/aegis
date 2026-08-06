#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cpp/common/clock.hpp"
#include "cpp/common/time.hpp"

/// Structured JSON Lines logging for the C++ side (AEGIS-232).
///
/// The peer of `python/common/logging.py`, emitting the same record shape
/// described by `configs/schemas/log_record.v1.json`. Four properties matter,
/// each answering a way logs stop being useful:
///
/// * **machine-readable** — one JSON object per line; free text cannot be
///   joined, filtered or diffed, so it cannot serve as evidence;
/// * **correlated** — every record carries `experiment_id`, the same field the
///   message envelope carries, plus an optional `correlation_id`;
/// * **deterministic** — the clock is injected and a per-logger sequence breaks
///   timestamp ties, so a fixture run twice produces identical bytes;
/// * **secret-free** — values are redacted by key name as the record is built,
///   because a logger is the most common way a credential reaches disk.
///
/// A logger is an instance. There is no global logger: a process-global sink
/// would be mutable state shared by every book partition, which is exactly what
/// the single-writer rule (AEGIS-047) exists to prevent.
namespace aegis::common {

inline constexpr int kLogSchemaVersion = 1;

enum class LogLevel : std::uint8_t {
  kTrace = 10,
  kDebug = 20,
  kInfo = 30,
  kWarn = 40,
  kError = 50,
};

[[nodiscard]] std::string_view to_string(LogLevel level);

/// Parse a level name. Returns nullopt rather than defaulting: silently
/// treating an unrecognised level as "info" hides a misconfigured run.
[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view name);

/// A structured field value. Scalars only — a nested blob defeats the point of
/// machine-readable logs and is unbounded work on a hot path.
using FieldValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

/// Fields in a record. Ordered, so two identical runs serialize identically.
using Fields = std::map<std::string, FieldValue>;

/// True when a field name looks like it holds a credential.
[[nodiscard]] bool is_secret_field(std::string_view name);

/// Where records go. A sink writes one line and does not interpret it.
class LogSink {
 public:
  LogSink() = default;
  LogSink(const LogSink&) = delete;
  LogSink& operator=(const LogSink&) = delete;
  LogSink(LogSink&&) = delete;
  LogSink& operator=(LogSink&&) = delete;
  virtual ~LogSink() = default;

  virtual void write_line(const std::string& line) = 0;
};

/// Collect records in memory. The sink tests and fixtures use.
class VectorLogSink final : public LogSink {
 public:
  void write_line(const std::string& line) override { lines_.push_back(line); }

  [[nodiscard]] const std::vector<std::string>& lines() const { return lines_; }

 private:
  std::vector<std::string> lines_;
};

/// Append records to a file, one JSON object per line.
class FileLogSink final : public LogSink {
 public:
  explicit FileLogSink(std::string path) : path_(std::move(path)) {}

  void write_line(const std::string& line) override;

 private:
  std::string path_;
};

/// A logger bound to one experiment, clock and sink.
class StructuredLogger {
 public:
  StructuredLogger(std::string name, std::string experiment_id, const WallClock& clock,
                   LogSink& sink, LogLevel level = LogLevel::kInfo,
                   std::string correlation_id = {});

  void log(LogLevel level, std::string_view message, const Fields& fields = {});

  void trace(std::string_view message, const Fields& fields = {}) {
    log(LogLevel::kTrace, message, fields);
  }
  void debug(std::string_view message, const Fields& fields = {}) {
    log(LogLevel::kDebug, message, fields);
  }
  void info(std::string_view message, const Fields& fields = {}) {
    log(LogLevel::kInfo, message, fields);
  }
  void warn(std::string_view message, const Fields& fields = {}) {
    log(LogLevel::kWarn, message, fields);
  }
  void error(std::string_view message, const Fields& fields = {}) {
    log(LogLevel::kError, message, fields);
  }

  /// Derive a child sharing this logger's clock, sink and experiment.
  ///
  /// The child starts its own sequence: sequence numbers order one emitter's
  /// records, and a shared counter would make that ordering depend on how
  /// components happened to interleave.
  [[nodiscard]] StructuredLogger bind(std::string name, std::string correlation_id) const;

  [[nodiscard]] std::uint64_t sequence() const { return sequence_; }
  [[nodiscard]] LogLevel level() const { return level_; }

 private:
  std::string name_;
  std::string experiment_id_;
  const WallClock* clock_;
  LogSink* sink_;
  LogLevel level_;
  std::string correlation_id_;
  std::uint64_t sequence_{0};
};

}  // namespace aegis::common
