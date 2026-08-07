# Layer cpp-exchange-state: journal, event log, snapshot codec — exchange
# *state*, accurately named rather than a vacuous fit inside matching or
# market data (ADR-0012). May depend on cpp-common, cpp-events,
# cpp-exchange-sequencer and cpp-exchange-order-book.
add_library(aegis_exchange_state
  event_log.cpp
  event_log.hpp)

target_include_directories(aegis_exchange_state PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_exchange_state
  PUBLIC aegis_common aegis_events aegis_exchange_sequencer aegis_exchange_order_book
  PRIVATE aegis_warnings)
