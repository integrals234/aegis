# Layer cpp-exchange-app: the composition root (ADR-0012). May depend on all
# other exchange layers — nothing else may legally see sequencer, book,
# matching and state at once.
add_library(aegis_exchange_app
  exchange_node.cpp
  exchange_node.hpp)

target_include_directories(aegis_exchange_app PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_exchange_app
  PUBLIC aegis_common aegis_events aegis_exchange_sequencer aegis_exchange_order_book
         aegis_exchange_matching aegis_exchange_state
  PRIVATE aegis_warnings)
