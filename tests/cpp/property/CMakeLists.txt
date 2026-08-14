aegis_add_test(aegis_property_tests property
  test_allocation_counters.cpp
  test_book_invariants_fuzz.cpp
  test_build_info_invariants.cpp
  test_match_visits.cpp
  test_order_index.cpp
  test_portfolio_conservation.cpp
  test_quantity_conservation.cpp)
target_link_libraries(aegis_property_tests PRIVATE
  aegis_common aegis_events
  aegis_exchange_order_book aegis_exchange_sequencer aegis_exchange_matching
  aegis_exchange_app
  aegis_participant_portfolio)
