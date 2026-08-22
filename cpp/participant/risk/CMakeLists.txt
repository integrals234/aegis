# Layer cpp-participant-risk (AEGIS-120..138; ADR-0027, ADR-0028).
# may_depend_on: [cpp-common, cpp-events, cpp-participant-strategy,
# cpp-statistics] -- no OMS, no portfolio, no book-builder, no exchange edge
# anywhere in this target. Populated M5 slice 1 under
# m5-architecture-transition (PR #13): cpp-participant-oms already depends on
# cpp-participant-risk, so the reverse edge would cycle -- this library
# implements no OMS interface itself; app::RiskEngineGate (cpp/participant/app)
# is the adapter.
add_library(aegis_participant_risk
  margin_model.cpp
  margin_model.hpp
  reason_codes.cpp
  reason_codes.hpp
  risk_audit.cpp
  risk_audit.hpp
  risk_engine.cpp
  risk_engine.hpp
  risk_limits.hpp
  risk_state.hpp
  risk_types.hpp)

target_include_directories(aegis_participant_risk PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_participant_risk
  PUBLIC aegis_common aegis_events aegis_statistics
  PRIVATE aegis_warnings)
