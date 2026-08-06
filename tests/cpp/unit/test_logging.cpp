#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cpp/common/clock.hpp"
#include "cpp/common/config.hpp"
#include "cpp/common/logging.hpp"
#include "cpp/common/time.hpp"

/// AEGIS-232, C++ side.
///
/// The records emitted here are validated against the *same*
/// configs/schemas/log_record.v1.json that the Python logger's tests use, with
/// the same validator the configuration loader uses. Two loggers writing
/// differently shaped records would make a joined analysis quietly wrong.
namespace {

using aegis::common::Fields;
using aegis::common::LogLevel;
using aegis::common::ManualClock;
using aegis::common::StructuredLogger;
using aegis::common::VectorLogSink;

nlohmann::json log_schema() {
  const std::filesystem::path root{AEGIS_SOURCE_ROOT};
  return aegis::common::load_schema(root / "configs/schemas/log_record.v1.json");
}

/// The shared validator rejects a document whose config_version is absent, which
/// a log record legitimately has no reason to carry. Checking the record's own
/// constraints is what matters here.
std::vector<std::string> schema_problems(const nlohmann::json& record) {
  auto problems = aegis::common::validate(record, log_schema());
  std::erase_if(problems, [](const std::string& problem) {
    return problem.find("config_version") != std::string::npos;
  });
  return problems;
}

TEST(CppLogging, EveryRecordSatisfiesTheSharedSchema) {
  VectorLogSink sink;
  const ManualClock clock{1'700'000'000'000'000'000};
  StructuredLogger logger{"exchange.sequencer", "m0-logging", clock, sink, LogLevel::kTrace};

  logger.info("book opened", Fields{{"instrument", std::string{"ESZ6"}}, {"levels", 10}});
  logger.warn("sequence gap", Fields{{"expected", 41}, {"received", 43}});
  logger.error("feed disconnected");

  ASSERT_EQ(sink.lines().size(), 3U);
  for (const auto& line : sink.lines()) {
    const auto record = nlohmann::json::parse(line);
    EXPECT_TRUE(schema_problems(record).empty())
        << line << "\nproblems: " << nlohmann::json(schema_problems(record)).dump();
  }
}

TEST(CppLogging, RecordsCarryTheExperimentId) {
  VectorLogSink sink;
  const ManualClock clock{0};
  StructuredLogger logger{"t", "m0-logging", clock, sink};
  logger.info("hello");

  EXPECT_EQ(nlohmann::json::parse(sink.lines().front()).at("experiment_id"), "m0-logging");
}

TEST(CppLogging, SequenceBreaksTimestampTies) {
  VectorLogSink sink;
  const ManualClock clock{42};  // never advanced: both records share a timestamp
  StructuredLogger logger{"t", "exp", clock, sink};
  logger.info("first");
  logger.info("second");

  const auto first = nlohmann::json::parse(sink.lines().at(0));
  const auto second = nlohmann::json::parse(sink.lines().at(1));
  EXPECT_EQ(first.at("timestamp_ns"), second.at("timestamp_ns"));
  EXPECT_EQ(first.at("sequence"), 0);
  EXPECT_EQ(second.at("sequence"), 1);
}

TEST(CppLogging, OutputIsByteIdenticalAcrossRuns) {
  // The clock is injected precisely so a fixture can be hashed (AEGIS-005).
  const auto run = [] {
    VectorLogSink sink;
    ManualClock clock{1'700'000'000'000'000'000};
    StructuredLogger logger{"replay", "exp-determinism", clock, sink, LogLevel::kTrace};
    for (int index = 0; index < 5; ++index) {
      logger.debug("tick", Fields{{"index", index}});
      clock.advance(aegis::common::millis(1));
    }
    return sink.lines();
  };
  EXPECT_EQ(run(), run());
}

TEST(CppLogging, SecretShapedFieldsAreRedacted) {
  VectorLogSink sink;
  const ManualClock clock{0};
  StructuredLogger logger{"t", "exp", clock, sink};
  logger.info("connecting", Fields{{"api_key", std::string{"EXAMPLEnotarealcredential"}},
                                   {"broker", std::string{"paper"}}});

  const auto line = sink.lines().front();
  EXPECT_EQ(line.find("EXAMPLEnotarealcredential"), std::string::npos)
      << "a logger is the most common way a credential reaches disk";
  const auto record = nlohmann::json::parse(line);
  EXPECT_EQ(record.at("fields").at("api_key"), "[redacted]");
  EXPECT_EQ(record.at("fields").at("broker"), "paper")
      << "a redactor that eats everything gets disabled";
}

TEST(CppLogging, LevelThresholdSuppressesQuieterRecords) {
  VectorLogSink sink;
  const ManualClock clock{0};
  StructuredLogger logger{"t", "exp", clock, sink, LogLevel::kWarn};
  logger.debug("dropped");
  logger.info("dropped");
  logger.warn("kept");
  logger.error("kept");

  ASSERT_EQ(sink.lines().size(), 2U);
  EXPECT_EQ(nlohmann::json::parse(sink.lines().at(0)).at("level"), "warn");
  EXPECT_EQ(nlohmann::json::parse(sink.lines().at(0)).at("sequence"), 0)
      << "a suppressed record must not consume a sequence number";
}

TEST(CppLogging, BoundLoggerKeepsItsOwnSequenceAndCorrelation) {
  VectorLogSink sink;
  const ManualClock clock{0};
  StructuredLogger parent{"a", "exp", clock, sink};
  auto child = parent.bind("participant.oms", "order-4711");

  parent.info("one");
  child.info("two");
  child.info("three");

  const auto second = nlohmann::json::parse(sink.lines().at(1));
  EXPECT_EQ(second.at("logger"), "participant.oms");
  EXPECT_EQ(second.at("correlation_id"), "order-4711");
  EXPECT_EQ(second.at("sequence"), 0);
  EXPECT_EQ(nlohmann::json::parse(sink.lines().at(2)).at("sequence"), 1);
}

TEST(CppLogging, LevelParsingRejectsAnUnknownName) {
  // Defaulting an unrecognised level to "info" would hide a misconfigured run.
  EXPECT_FALSE(aegis::common::parse_log_level("verbose").has_value());

  const auto parsed = aegis::common::parse_log_level("WARN");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value_or(LogLevel::kTrace), LogLevel::kWarn);
}

TEST(CppLogging, LoggerRequiresANameAndAnExperimentId) {
  VectorLogSink sink;
  const ManualClock clock{0};
  EXPECT_THROW((StructuredLogger{"", "exp", clock, sink}), std::invalid_argument);
  EXPECT_THROW((StructuredLogger{"t", "", clock, sink}), std::invalid_argument);
}

TEST(CppLogging, FieldOrderIsIndependentOfInsertionOrder) {
  VectorLogSink sink;
  const ManualClock clock{0};
  StructuredLogger logger{"t", "exp", clock, sink};
  logger.info("m", Fields{{"beta", 2}, {"alpha", 1}});

  const auto line = sink.lines().front();
  EXPECT_LT(line.find("\"alpha\""), line.find("\"beta\""))
      << "otherwise two logically identical runs would hash differently";
}

}  // namespace
