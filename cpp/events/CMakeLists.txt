# Layer cpp-events: the versioned message envelope plus the exchange domain
# and market-data message vocabularies (ADR-0009, ADR-0020). Depends on
# cpp-common only (configs/architecture_rules.yaml); this is what lets the M3
# participant decode exchange and market-data events without depending on the
# exchange layer at all.
add_library(aegis_events
  envelope.cpp
  envelope.hpp
  exchange_messages.cpp
  exchange_messages.hpp
  market_data_messages.cpp
  market_data_messages.hpp
  sequence.hpp
  wire.cpp
  wire.hpp)

target_include_directories(aegis_events PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_events
  PUBLIC aegis_common
  PRIVATE aegis_warnings)
