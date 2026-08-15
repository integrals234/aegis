# Layer cpp-exchange-market-data (AEGIS-064, AEGIS-065; ADR-0012, ADR-0020).
# Re-dated M1 -> M3: the first genuine consumer is the M3 participant. Observes
# OrderBook and the EmittedEvent stream; never calls into matching, never
# mutates a book.
add_library(aegis_exchange_market_data
  market_data_publisher.cpp
  market_data_publisher.hpp)

target_include_directories(aegis_exchange_market_data PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_exchange_market_data
  PUBLIC aegis_common aegis_events aegis_exchange_order_book
  PRIVATE aegis_warnings)
