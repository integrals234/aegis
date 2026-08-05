#include "cpp/common/version.hpp"

#include <sstream>
#include <string>

namespace aegis::common {
namespace {

constexpr const char* kVersion = AEGIS_VERSION;
constexpr const char* kCompiler = AEGIS_CXX_COMPILER_ID;
constexpr const char* kCompilerVersion = AEGIS_CXX_COMPILER_VERSION;
constexpr const char* kBuildType = AEGIS_BUILD_TYPE;

constexpr bool kAssertions =
#ifdef NDEBUG
    false;
#else
    true;
#endif

constexpr bool kSanitizers =
#ifdef __SANITIZE_ADDRESS__
    true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer) || \
    __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
    true;
#else
    false;
#endif
#else
        false;
#endif

}  // namespace

std::string version() { return kVersion; }

bool assertions_enabled() { return kAssertions; }

bool sanitizers_enabled() { return kSanitizers; }

std::string build_info() {
  std::ostringstream out;
  out << "aegis " << kVersion << "; compiler " << kCompiler << ' ' << kCompilerVersion
      << "; std c++" << __cplusplus / 100 % 100 << "; build " << kBuildType << "; assertions "
      << (kAssertions ? "on" : "off") << "; sanitizers " << (kSanitizers ? "on" : "off");
  return out.str();
}

}  // namespace aegis::common
