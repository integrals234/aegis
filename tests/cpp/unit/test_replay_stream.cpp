#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "cpp/replay/replay_stream.hpp"

// ---------------------------------------------------------------------------
// M2 slice 9 -- loading and validating a canonical replay input file.
// ---------------------------------------------------------------------------

namespace {

using aegis::replay::load_replay_stream;
using aegis::replay::ReplayStreamError;

std::filesystem::path write_temp_file(const std::string& name, const std::string& content) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream file(path);
  file << content;
  file.close();
  return path;
}

constexpr auto kValidStream =
    "{\"event_time_ns\":1000,\"source_sequence\":1,\"contract_symbol\":\"SYNX:EQX:2026H\","
    "\"record_index\":0}\n"
    "{\"event_time_ns\":1000,\"source_sequence\":2,\"contract_symbol\":\"SYNX:EQX:2026H\","
    "\"record_index\":1}\n"
    "{\"event_time_ns\":1001,\"source_sequence\":1,\"contract_symbol\":\"SYNX:EQX:2026H\","
    "\"record_index\":2}\n";

TEST(ReplayStream, LoadsAValidCanonicallyOrderedFile) {
  const auto path = write_temp_file("aegis_replay_stream_valid.jsonl", kValidStream);
  const auto result = load_replay_stream(path.string());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().size(), 3U);
  EXPECT_EQ(result.value()[0].record_index.value(), 0U);
  EXPECT_EQ(result.value()[2].source_sequence.value(), 1U);
  std::filesystem::remove(path);
}

TEST(ReplayStream, SkipsBlankLines) {
  const auto path = write_temp_file("aegis_replay_stream_blank.jsonl",
                                    "\n{\"event_time_ns\":1,\"source_sequence\":1,\"contract_"
                                    "symbol\":\"A\",\"record_index\":0}\n\n");
  const auto result = load_replay_stream(path.string());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().size(), 1U);
  std::filesystem::remove(path);
}

TEST(ReplayStream, MissingFileIsReported) {
  const auto result = load_replay_stream("/nonexistent/aegis_replay_stream_missing.jsonl");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReplayStreamError::kFileNotFound);
}

TEST(ReplayStream, MalformedRecordIsReported) {
  const auto path =
      write_temp_file("aegis_replay_stream_malformed.jsonl", "{\"event_time_ns\":1}\n");
  const auto result = load_replay_stream(path.string());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReplayStreamError::kMalformedRecord);
  std::filesystem::remove(path);
}

TEST(ReplayStream, OutOfOrderRecordIsReported) {
  const auto path = write_temp_file("aegis_replay_stream_out_of_order.jsonl",
                                    "{\"event_time_ns\":1000,\"source_sequence\":1,\"contract_"
                                    "symbol\":\"A\",\"record_index\":0}\n"
                                    "{\"event_time_ns\":999,\"source_sequence\":1,\"contract_"
                                    "symbol\":\"A\",\"record_index\":1}\n");
  const auto result = load_replay_stream(path.string());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReplayStreamError::kOutOfOrder);
  std::filesystem::remove(path);
}

TEST(ReplayStream, DuplicateCanonicalKeyIsReported) {
  const auto path = write_temp_file("aegis_replay_stream_duplicate.jsonl",
                                    "{\"event_time_ns\":1000,\"source_sequence\":1,\"contract_"
                                    "symbol\":\"A\",\"record_index\":0}\n"
                                    "{\"event_time_ns\":1000,\"source_sequence\":1,\"contract_"
                                    "symbol\":\"A\",\"record_index\":0}\n");
  const auto result = load_replay_stream(path.string());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ReplayStreamError::kDuplicateKey);
  std::filesystem::remove(path);
}

TEST(ReplayStream, ValueOnFailedLoadThrows) {
  const auto result = load_replay_stream("/nonexistent/aegis_replay_stream_missing2.jsonl");
  EXPECT_THROW({ [[maybe_unused]] const auto& value = result.value(); }, std::runtime_error);
}

}  // namespace
