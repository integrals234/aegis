# AEGIS C++ build (AEGIS-227).
#
# Correctness first: warnings are errors, sanitizers are a first-class preset,
# and no optimization work is permitted before a correct baseline exists
# (AEGIS-005, AEGIS-051). Third-party code is acquired at a pinned tag *and* a
# pinned archive hash, so a retagged upstream release cannot silently change
# what AEGIS was tested against.
cmake_minimum_required(VERSION 3.24)

project(aegis
  VERSION 0.1.0
  DESCRIPTION "AEGIS futures research, exchange-simulation, risk and decision-intelligence platform"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AEGIS_BUILD_TESTS "Build the C++ test layers" ON)
option(AEGIS_BUILD_BINDINGS "Build the pybind11 bindings module" ON)
option(AEGIS_WERROR "Treat warnings as errors" ON)

add_library(aegis_warnings INTERFACE)
target_compile_options(aegis_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wsign-conversion
  -Wshadow
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Woverloaded-virtual
  -Wcast-align
  -Wunused
  -Wdouble-promotion
  -Wformat=2)
if(AEGIS_WERROR)
  target_compile_options(aegis_warnings INTERFACE -Werror)
endif()

add_subdirectory(cpp)

if(AEGIS_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests/cpp)
endif()
