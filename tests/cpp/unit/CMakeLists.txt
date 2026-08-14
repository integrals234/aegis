aegis_add_test(aegis_unit_tests unit
  test_adapter_contract.cpp
  test_book_add_cancel.cpp
  test_book_builder.cpp
  test_book_invariants.cpp
  test_client_order_id_scope.cpp
  test_config.cpp
  test_cost_model.cpp
  test_envelope.cpp
  test_event_log.cpp
  test_exchange_messages.cpp
  test_exchange_node.cpp
  test_exchange_types.cpp
  test_execution_adapter.cpp
  test_fault_injection.cpp
  test_feed_handler.cpp
  test_full_fills.cpp
  test_instrument_grid.cpp
  test_level_index.cpp
  test_latency_model.cpp
  test_limit_orders.cpp
  test_logging.cpp
  test_market_data_messages.cpp
  test_market_data_publisher.cpp
  test_market_orders.cpp
  test_market_stress_faults.cpp
  test_matching_fifo.cpp
  test_matching_policy_interface.cpp
  test_missed_trade_tracker.cpp
  test_modify_semantics.cpp
  test_order_lifecycle.cpp
  test_order_manager.cpp
  test_pacing.cpp
  test_participant_exchange_integration.cpp
  test_participant_run.cpp
  test_portfolio.cpp
  test_queue_position_estimator.cpp
  test_recorded_response_adapter.cpp
  test_reject_matrix.cpp
  test_replay_engine.cpp
  test_replay_event.cpp
  test_replay_full_stack_integration.cpp
  test_replay_manifest.cpp
  test_replay_stream.cpp
  test_drawdown_tracker.cpp
  test_exponential_stats.cpp
  test_realized_volatility.cpp
  test_rolling_covariance.cpp
  test_rolling_moments.cpp
  test_rolling_zscore.cpp
  test_feed_recovery.cpp
  test_intensity_tracker.cpp
  test_rolling_rate.cpp
  test_sequencer.cpp
  test_sequence_tracker.cpp
  test_snapshot_roundtrip.cpp
  test_stale_data_response.cpp
  test_time.cpp
  test_version.cpp
  test_virtual_clock.cpp)
target_link_libraries(aegis_unit_tests PRIVATE
  aegis_common aegis_events
  aegis_exchange_order_book aegis_exchange_sequencer aegis_exchange_matching
  aegis_exchange_state aegis_exchange_market_data aegis_exchange_app
  aegis_replay
  aegis_participant_feed_handler aegis_participant_book_builder
  aegis_statistics aegis_participant_oms aegis_participant_portfolio
  aegis_participant_app)

# The config tests read the schema and corpus from the source tree at runtime;
# the exchange-message golden fixture and the exchange determinism lint do too.
target_compile_definitions(aegis_unit_tests PRIVATE AEGIS_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
