#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cpp/common/config.hpp"

/// AEGIS-231, C++ side.
///
/// These tests read the *same* corpus and the *same* schema file as
/// tests/unit/test_config_validation.py. That is the point: two loaders with
/// private copies of the rules drift, and the drift shows up as a run that
/// behaved differently from the configuration describing it. If the two
/// implementations ever disagree about a fixture, one of these suites fails.
namespace {

using aegis::common::Config;
using aegis::common::ConfigError;

std::filesystem::path repo_root() { return std::filesystem::path{AEGIS_SOURCE_ROOT}; }

std::filesystem::path schema_path() { return repo_root() / "configs/schemas/config.v1.json"; }

std::filesystem::path corpus() { return repo_root() / "tests/unit/fixtures/configs"; }

nlohmann::json read_json(const std::filesystem::path& path) {
  std::ifstream input{path};
  EXPECT_TRUE(input.good()) << "cannot open " << path;
  return nlohmann::json::parse(input);
}

std::vector<std::filesystem::path> files_in(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator{directory}) {
    if (entry.path().extension() == ".json") {
      paths.push_back(entry.path());
    }
  }
  std::ranges::sort(paths);
  return paths;
}

TEST(Config, AcceptsEveryValidFixture) {
  const auto paths = files_in(corpus() / "valid");
  ASSERT_FALSE(paths.empty()) << "the corpus must not be empty, or this test asserts nothing";
  for (const auto& path : paths) {
    EXPECT_NO_THROW({
      const Config config = aegis::common::load_config(path, schema_path());
      EXPECT_EQ(config.config_version(), aegis::common::kConfigSchemaVersion);
      EXPECT_FALSE(config.experiment_id().empty());
    }) << path;
  }
}

TEST(Config, RejectsEveryInvalidFixtureNamingTheField) {
  // The expectations file states which field each rejection must mention.
  // "Invalid configs fail with clear errors" is the acceptance criterion, and an
  // error that does not name the offending field is not a clear one.
  const auto expectations = read_json(corpus() / "expectations.json").at("invalid");
  const auto paths = files_in(corpus() / "invalid");
  ASSERT_EQ(paths.size(), expectations.size());

  for (const auto& path : paths) {
    const auto name = path.filename().string();
    ASSERT_TRUE(expectations.contains(name)) << name << " has no recorded expectation";
    const auto expected = expectations.at(name).get<std::string>();

    try {
      [[maybe_unused]] const auto accepted = aegis::common::load_config(path, schema_path());
      ADD_FAILURE() << name << " was accepted but must be rejected";
    } catch (const ConfigError& error) {
      const std::string message = error.what();
      EXPECT_NE(message.find(expected), std::string::npos)
          << name << " was rejected, but the message does not mention '" << expected
          << "': " << message;
    }
  }
}

TEST(Config, MissingConfigVersionIsRejected) {
  const auto schema = aegis::common::load_schema(schema_path());
  const auto problems = aegis::common::validate(
      nlohmann::json::parse(R"({"run": {"experiment_id": "x", "seed": 1}})"), schema);
  ASSERT_FALSE(problems.empty());
  EXPECT_NE(problems.front().find("config_version"), std::string::npos);
}

TEST(Config, FutureVersionIsRejectedRatherThanReinterpreted) {
  const auto schema = aegis::common::load_schema(schema_path());
  const auto document = nlohmann::json::parse(
      R"({"config_version": 2, "run": {"experiment_id": "x", "seed": 1},
          "logging": {"level": "info", "format": "jsonl"}})");
  const auto problems = aegis::common::validate(document, schema);
  ASSERT_FALSE(problems.empty());
  EXPECT_NE(problems.front().find("not supported by this build"), std::string::npos);
}

TEST(Config, ReportsEveryProblemNotOnlyTheFirst) {
  const auto schema = aegis::common::load_schema(schema_path());
  const auto document = nlohmann::json::parse(
      R"({"config_version": 1, "run": {"experiment_id": "", "seed": -5},
          "logging": {"level": "verbose", "format": "jsonl"}})");
  const auto problems = aegis::common::validate(document, schema);
  EXPECT_GE(problems.size(), 3U) << "fixing a config one error per run wastes an afternoon";
}

TEST(Config, UnknownSchemaKeywordIsReportedRatherThanIgnored) {
  // A validator that skips keywords it does not understand accepts documents the
  // schema author meant to reject, while still looking like it is validating.
  const auto schema = nlohmann::json::parse(
      R"({"type": "object", "properties": {"a": {"type": "integer", "multipleOf": 3}}})");
  const auto problems =
      aegis::common::validate(nlohmann::json::parse(R"({"config_version": 1, "a": 4})"), schema);
  const bool reported = std::ranges::any_of(problems, [](const std::string& p) {
    return p.find("unsupported keyword 'multipleOf'") != std::string::npos;
  });
  EXPECT_TRUE(reported);
}

TEST(Config, MalformedJsonNamesItsOrigin) {
  const auto schema = aegis::common::load_schema(schema_path());
  try {
    [[maybe_unused]] const auto parsed =
        aegis::common::load_config_from_string("{not json", schema, "run.json");
    ADD_FAILURE() << "malformed JSON must be rejected";
  } catch (const ConfigError& error) {
    EXPECT_NE(std::string{error.what()}.find("run.json"), std::string::npos);
  }
}

TEST(Config, MissingFileIsReportedByPath) {
  EXPECT_THROW(std::ignore = aegis::common::load_config(corpus() / "absent.json", schema_path()),
               ConfigError);
}

TEST(Config, MissingSchemaIsReportedByPath) {
  EXPECT_THROW(
      std::ignore = aegis::common::load_schema(repo_root() / "configs/schemas/absent.json"),
      ConfigError);
}

TEST(Config, DottedLookupReturnsNullptrForAbsentPaths) {
  const Config config = aegis::common::load_config(corpus() / "valid/full.json", schema_path());
  ASSERT_NE(config.find("logging.level"), nullptr);
  EXPECT_EQ(config.find("logging.level")->get<std::string>(), "debug");
  EXPECT_EQ(config.find("logging.absent"), nullptr);
  EXPECT_EQ(config.find("absent.section.key"), nullptr);
}

TEST(Config, CanonicalJsonIsStableAcrossLoads) {
  // The resolved configuration is hashed into the experiment manifest, so its
  // serialization must not depend on the order keys happened to appear in.
  const auto first = aegis::common::load_config(corpus() / "valid/full.json", schema_path());
  const auto second = aegis::common::load_config(corpus() / "valid/full.json", schema_path());
  EXPECT_EQ(first.canonical_json(), second.canonical_json());
}

}  // namespace
