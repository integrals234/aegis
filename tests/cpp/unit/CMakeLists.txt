aegis_add_test(aegis_unit_tests unit
  test_config.cpp
  test_logging.cpp
  test_time.cpp
  test_version.cpp)
target_link_libraries(aegis_unit_tests PRIVATE aegis_common)

# The config tests read the schema and corpus from the source tree at runtime.
target_compile_definitions(aegis_unit_tests PRIVATE AEGIS_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
