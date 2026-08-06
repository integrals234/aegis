# Layer cpp-bindings: the only place pybind11 and Python.h may appear
# (configs/architecture_rules.yaml enforces it). Depends on cpp-common and
# cpp-events; it has no access to exchange or participant code, so a binding
# cannot route around the strategy -> risk -> OMS path.
#
# pybind11 comes from the pinned Python environment (requirements/requirements.lock)
# rather than a second, separately versioned copy fetched here.
find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)

if(NOT Python3_FOUND)
  message(STATUS "Python development headers not found; skipping aegis_bindings (AEGIS-229)")
  return()
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
  OUTPUT_VARIABLE AEGIS_PYBIND11_CMAKEDIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE AEGIS_PYBIND11_QUERY)

if(NOT AEGIS_PYBIND11_QUERY EQUAL 0)
  message(STATUS "pybind11 not importable from ${Python3_EXECUTABLE}; skipping aegis_bindings")
  return()
endif()

find_package(pybind11 CONFIG REQUIRED PATHS "${AEGIS_PYBIND11_CMAKEDIR}" NO_DEFAULT_PATH)

pybind11_add_module(aegis_bindings aegis_module.cpp)
target_include_directories(aegis_bindings PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_bindings PRIVATE aegis_common aegis_events)
