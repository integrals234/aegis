# Layer cpp-participant-app: the participant composition root (ADR-0020,
# ADR-0025, ADR-0027). May depend on every other participant layer plus
# cpp-replay -- nothing else may legally see feed handler, book builder,
# statistics, strategy, risk, OMS and portfolio at once. No edge to any
# cpp-exchange-* layer. cpp-replay is PUBLIC: fault_scenario.hpp's public
# signature names replay::ReplayEvent/FaultRule (AEGIS-060/061, ADR-0021) --
# this is the only participant layer permitted that edge.
# aegis_participant_strategy joined M4 slice 1 under
# m4-architecture-transition/m4-build-wiring (PR #10). aegis_participant_risk
# joined M5 slice 1 under m5-architecture-transition/m5-build-wiring (PR #13):
# risk_engine_gate.{hpp,cpp} is the oms::RiskGate adapter -- the only legal
# place for it, since cpp-participant-risk cannot depend on cpp-participant-oms
# (the reverse edge already exists and would cycle).
add_library(aegis_participant_app
  fault_scenario.cpp
  fault_scenario.hpp
  intensity_tracker.cpp
  intensity_tracker.hpp
  participant_run.cpp
  participant_run.hpp
  participant_snapshot.cpp
  participant_snapshot.hpp
  risk_engine_gate.cpp
  risk_engine_gate.hpp)

target_include_directories(aegis_participant_app PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_participant_app
  PUBLIC aegis_common aegis_events aegis_replay aegis_participant_feed_handler
         aegis_statistics
  PRIVATE aegis_participant_book_builder aegis_participant_strategy aegis_participant_risk
          aegis_participant_oms aegis_participant_portfolio nlohmann_json::nlohmann_json
          aegis_warnings)

# aegis_participant_run: this layer's CLI entry point, the participant-side
# counterpart to aegis_exchange_replay and aegis_replay_run.
add_executable(aegis_participant_run participant_run_main.cpp)
target_link_libraries(aegis_participant_run
  PRIVATE aegis_participant_app nlohmann_json::nlohmann_json aegis_warnings)
