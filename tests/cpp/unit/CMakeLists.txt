aegis_add_test(aegis_unit_tests unit
  test_book_add_cancel.cpp
  test_book_invariants.cpp
  test_client_order_id_scope.cpp
  test_config.cpp
  test_envelope.cpp
  test_event_log.cpp
  test_exchange_messages.cpp
  test_exchange_node.cpp
  test_exchange_types.cpp
  test_full_fills.cpp
  test_instrument_grid.cpp
  test_level_index.cpp
  test_limit_orders.cpp
  test_logging.cpp
  test_market_orders.cpp
  test_matching_fifo.cpp
  test_matching_policy_interface.cpp
  test_modify_semantics.cpp
  test_reject_matrix.cpp
  test_replay_event.cpp
  test_sequencer.cpp
  test_snapshot_roundtrip.cpp
  test_time.cpp
  test_version.cpp)
target_link_libraries(aegis_unit_tests PRIVATE
  aegis_common aegis_events
  aegis_exchange_order_book aegis_exchange_sequencer aegis_exchange_matching
  aegis_exchange_state aegis_exchange_app
  aegis_replay)

# The config tests read the schema and corpus from the source tree at runtime;
# the exchange-message golden fixture and the exchange determinism lint do too.
target_compile_definitions(aegis_unit_tests PRIVATE AEGIS_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
