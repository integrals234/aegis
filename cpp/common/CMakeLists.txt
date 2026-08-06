# Layer cpp-common: the substrate every other layer may depend on and which
# depends on nothing (configs/architecture_rules.yaml).
add_library(aegis_common
  clock.cpp
  clock.hpp
  time.hpp
  version.cpp
  version.hpp)

target_include_directories(aegis_common PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_common PRIVATE aegis_warnings)

# Build facts are compiled in rather than described in prose, so a binary can
# always report the toolchain that produced it (AEGIS-009, AEGIS-053).
target_compile_definitions(aegis_common PRIVATE
  AEGIS_VERSION="${PROJECT_VERSION}"
  AEGIS_CXX_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
  AEGIS_CXX_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
  AEGIS_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
