#include <string>

#include <gtest/gtest.h>

#include "cpp/common/version.hpp"

namespace {

TEST(Version, ReportsTheProjectVersion) {
  const std::string version = aegis::common::version();
  EXPECT_FALSE(version.empty());
  EXPECT_NE(version.find('.'), std::string::npos) << "version must be dotted, got " << version;
}

TEST(BuildInfo, DisclosesTheToolchainThatProducedTheBinary) {
  // docs/BENCHMARK_POLICY.md requires compiler, build type, assertion and
  // sanitizer disclosure alongside any performance figure. A build that cannot
  // report these cannot support such a figure.
  const std::string info = aegis::common::build_info();
  EXPECT_NE(info.find("compiler"), std::string::npos) << info;
  EXPECT_NE(info.find("build"), std::string::npos) << info;
  EXPECT_NE(info.find("assertions"), std::string::npos) << info;
  EXPECT_NE(info.find("sanitizers"), std::string::npos) << info;
}

TEST(BuildInfo, AssertionStateMatchesTheBuild) {
#ifdef NDEBUG
  EXPECT_FALSE(aegis::common::assertions_enabled());
#else
  EXPECT_TRUE(aegis::common::assertions_enabled());
#endif
}

}  // namespace
