aegis_add_test(aegis_unit_tests unit
  test_version.cpp)
target_link_libraries(aegis_unit_tests PRIVATE aegis_common)
