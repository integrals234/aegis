# AEGIS C++ test layers (AEGIS-233).
#
# GoogleTest is pinned by tag *and* by archive hash. A tag can be moved; a
# SHA-256 cannot, so this is the difference between "we test against v1.17.0"
# and "we test against whatever v1.17.0 points at today". Offline builds set
# FETCHCONTENT_FULLY_DISCONNECTED=ON with a populated FETCHCONTENT_BASE_DIR;
# see docs/ENVIRONMENT.md.

include(FetchContent)

set(AEGIS_GOOGLETEST_VERSION 1.17.0)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v${AEGIS_GOOGLETEST_VERSION}.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Each layer of tests/cpp maps to the identically-named pytest marker and ctest
# label, so every layer can be reported separately (AEGIS-233).
function(aegis_add_test name layer)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE GTest::gtest_main aegis_warnings)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR})
  gtest_discover_tests(${name} PROPERTIES LABELS ${layer})
endfunction()

add_subdirectory(unit)
add_subdirectory(property)
