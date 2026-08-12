# Layer cpp-participant-oms (AEGIS-108, AEGIS-119; ADR-0023). May depend on
# cpp-common, cpp-events and cpp-participant-risk (M5, empty) only -- no book,
# statistics, or exchange edge. allows_gateway_adapters: true is what lets
# this layer, and only this layer, hold the ExecutionAdapter/ExecutionTransport
# seam.
add_library(aegis_participant_oms
  execution_adapter.hpp
  execution_transport.hpp
  order_lifecycle.hpp
  order_state.cpp
  order_state.hpp
  risk_gate.hpp
  transport_execution_adapter.cpp
  transport_execution_adapter.hpp)

target_include_directories(aegis_participant_oms PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_participant_oms
  PUBLIC aegis_common aegis_events
  PRIVATE aegis_warnings)
