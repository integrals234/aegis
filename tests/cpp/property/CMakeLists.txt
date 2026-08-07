aegis_add_test(aegis_property_tests property
  test_allocation_counters.cpp
  test_build_info_invariants.cpp
  test_match_visits.cpp
  test_order_index.cpp)
target_link_libraries(aegis_property_tests PRIVATE
  aegis_common aegis_exchange_order_book aegis_exchange_matching)
