#include <string>

#include <gtest/gtest.h>

#include "cpp/common/version.hpp"

namespace {

// The property layer checks invariants over many inputs rather than one case.
// Here the "input" is repeated invocation: build_info() feeds environment
// capture and, later, benchmark disclosure, so it must be a pure function of
// the build rather than of when it is called (AEGIS-005).
TEST(BuildInfoProperty, IsStableAcrossRepeatedCalls) {
  const std::string first = aegis::common::build_info();
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(first, aegis::common::build_info());
  }
}

TEST(VersionProperty, IsStableAcrossRepeatedCalls) {
  const std::string first = aegis::common::version();
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(first, aegis::common::version());
  }
}

}  // namespace
