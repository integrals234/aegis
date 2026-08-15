# Layer cpp-exchange-app: the composition root (ADR-0012, ADR-0020). May
# depend on all other exchange layers — nothing else may legally see
# sequencer, book, matching, state and market-data at once.
add_library(aegis_exchange_app
  exchange_node.cpp
  exchange_node.hpp)

target_include_directories(aegis_exchange_app PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_exchange_app
  PUBLIC aegis_common aegis_events aegis_exchange_sequencer aegis_exchange_order_book
         aegis_exchange_matching aegis_exchange_state aegis_exchange_market_data
  PRIVATE aegis_warnings)

# aegis_exchange_replay: the CLI entry point python/common/determinism.py's
# exchange_producer and the AEGIS-005 cross-process determinism harness both
# invoke (ADR-0012, ADR-0013). The only exchange-side executable, so it is
# the only exchange target that needs the JSON fixture reader.
add_executable(aegis_exchange_replay replay_main.cpp)
target_link_libraries(aegis_exchange_replay
  PRIVATE aegis_exchange_app nlohmann_json::nlohmann_json aegis_warnings)

# aegis_exchange_bench: the three named workloads plus the BENCHMARK_POLICY
# required mix (AEGIS-036, AEGIS-039, §4.7). Only this target links
# cpp/common's real SystemSteadyClock for latency measurement — every other
# exchange layer takes an injected clock or none at all.
add_executable(aegis_exchange_bench bench_main.cpp)
target_link_libraries(aegis_exchange_bench
  PRIVATE aegis_exchange_app nlohmann_json::nlohmann_json aegis_warnings)
