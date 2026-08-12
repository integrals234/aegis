# Layer cpp-participant-app: the participant composition root (ADR-0020).
# May depend on every other participant layer plus cpp-replay -- nothing else
# may legally see feed handler, book builder, statistics, OMS and portfolio
# at once. No edge to any cpp-exchange-* layer.
add_library(aegis_participant_app
  participant_run.cpp
  participant_run.hpp)

target_include_directories(aegis_participant_app PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_participant_app
  PUBLIC aegis_common aegis_events
  PRIVATE aegis_participant_feed_handler aegis_participant_book_builder
          aegis_statistics aegis_participant_oms aegis_participant_portfolio
          aegis_warnings)

# aegis_participant_run: this layer's CLI entry point, the participant-side
# counterpart to aegis_exchange_replay and aegis_replay_run.
add_executable(aegis_participant_run participant_run_main.cpp)
target_link_libraries(aegis_participant_run
  PRIVATE aegis_participant_app nlohmann_json::nlohmann_json aegis_warnings)
