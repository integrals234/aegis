# Layer cpp-bindings: the only place pybind11 and Python.h may appear
# (configs/architecture_rules.yaml enforces it). Depends on cpp-common,
# cpp-events, (M2 slice 13) cpp-replay and (M3 slice 4, AEGIS-107)
# cpp-statistics; it has no access to exchange or participant code, so a
# binding cannot route around the strategy -> risk -> OMS path.
#
# pybind11 comes from the pinned Python environment (requirements/requirements.lock)
# rather than a second, separately versioned copy fetched here.
# Skipping this module is legitimate on a machine with no Python development
# headers, but it must never be *silent* in CI: without the target there is no
# compile_commands.json entry for aegis_module.cpp, and clang-tidy then
# analyses it with fallback flags and reports a cascade of diagnostics about
# the resulting broken parse instead of about the code. AEGIS_REQUIRE_BINDINGS
# turns the skip into a configure-time failure that names the real cause;
# .github/workflows/ci.yml sets it, so CI cannot quietly lose the module.
option(AEGIS_REQUIRE_BINDINGS
  "Fail configuration if the pybind11 module cannot be built (set in CI)" OFF)

function(aegis_bindings_unavailable reason)
  if(AEGIS_REQUIRE_BINDINGS)
    message(FATAL_ERROR
      "aegis_bindings cannot be configured: ${reason}. "
      "AEGIS_REQUIRE_BINDINGS is ON, so this is an error rather than a skip — "
      "without the target, cpp/bindings/aegis_module.cpp has no compile command "
      "and every tool that reads compile_commands.json misreports it.")
  endif()
  message(WARNING "aegis_bindings skipped: ${reason} (AEGIS-229)")
endfunction()

find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)

if(NOT Python3_FOUND)
  aegis_bindings_unavailable("Python interpreter or development headers not found")
  return()
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
  OUTPUT_VARIABLE AEGIS_PYBIND11_CMAKEDIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE AEGIS_PYBIND11_QUERY)

if(NOT AEGIS_PYBIND11_QUERY EQUAL 0)
  aegis_bindings_unavailable("pybind11 is not importable from ${Python3_EXECUTABLE}")
  return()
endif()

find_package(pybind11 CONFIG REQUIRED PATHS "${AEGIS_PYBIND11_CMAKEDIR}" NO_DEFAULT_PATH)

pybind11_add_module(aegis_bindings aegis_module.cpp)
target_include_directories(aegis_bindings PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_bindings PRIVATE aegis_common aegis_events aegis_replay aegis_statistics)
