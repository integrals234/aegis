#include "cpp/replay/replay_stream.hpp"

#include <compare>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace aegis::replay {

namespace {
using Json = nlohmann::json;
}  // namespace

std::string describe(ReplayStreamError error) {
  switch (error) {
    case ReplayStreamError::kFileNotFound:
      return "input file not found or could not be opened";
    case ReplayStreamError::kMalformedRecord:
      return "a record is missing a required field or has the wrong type";
    case ReplayStreamError::kOutOfOrder:
      return "a record is not canonically greater than the one before it";
    case ReplayStreamError::kDuplicateKey:
      return "two records compare canonically equal (a record_index collision)";
  }
  return "unknown replay stream error";  // pragma: exhaustive enum above
}

ReplayStreamResult ReplayStreamResult::success(std::vector<ReplayEvent> events) {
  ReplayStreamResult result;
  result.value_ = std::move(events);
  return result;
}

ReplayStreamResult ReplayStreamResult::failure(ReplayStreamError error, std::string detail) {
  ReplayStreamResult result;
  result.error_ = error;
  result.detail_ = std::move(detail);
  return result;
}

const std::vector<ReplayEvent>& ReplayStreamResult::value() const {
  if (!value_.has_value()) {
    throw std::runtime_error("ReplayStreamResult::value() called on a failed load: " +
                             describe(error_) + ": " + detail_);
  }
  return *value_;  // NOLINT(bugprone-unchecked-optional-access) - guarded above
}

namespace {

/// Parses one JSON-Lines record into a ReplayEvent, or throws
/// nlohmann::json::exception / std::out_of_range on a missing/malformed
/// field -- caught by the caller and turned into `kMalformedRecord`.
ReplayEvent parse_record(const Json& record) {
  ReplayEvent event;
  event.event_time = common::EventTime{record.at("event_time_ns").get<std::int64_t>()};
  event.source_sequence = SourceSequence{record.at("source_sequence").get<std::uint64_t>()};
  event.contract_symbol = record.at("contract_symbol").get<std::string>();
  event.record_index = RecordIndex{record.at("record_index").get<std::uint64_t>()};
  return event;
}

}  // namespace

ReplayStreamResult load_replay_stream(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    return ReplayStreamResult::failure(ReplayStreamError::kFileNotFound, path);
  }

  std::vector<ReplayEvent> events;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
      continue;  // blank lines are skipped, not malformed
    }
    try {
      const auto record = Json::parse(line);
      events.push_back(parse_record(record));
    } catch (const Json::exception& parse_error) {
      std::ostringstream detail;
      detail << path << ":" << line_number << ": " << parse_error.what();
      return ReplayStreamResult::failure(ReplayStreamError::kMalformedRecord, detail.str());
    }
  }

  for (std::size_t i = 1; i < events.size(); ++i) {
    const auto order = canonical_compare(events[i - 1], events[i]);
    if (order == std::strong_ordering::equal) {
      std::ostringstream detail;
      detail << path << ": records at positions " << (i - 1) << " and " << i
             << " compare canonically equal (record_index " << events[i].record_index.value()
             << ")";
      return ReplayStreamResult::failure(ReplayStreamError::kDuplicateKey, detail.str());
    }
    if (order == std::strong_ordering::greater) {
      std::ostringstream detail;
      detail << path << ": record at position " << i
             << " is not canonically greater than the one before it";
      return ReplayStreamResult::failure(ReplayStreamError::kOutOfOrder, detail.str());
    }
  }

  return ReplayStreamResult::success(std::move(events));
}

}  // namespace aegis::replay
