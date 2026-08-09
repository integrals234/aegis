# Layer cpp-exchange-order-book: the central limit order book. May depend on
# cpp-common and cpp-events only (configs/architecture_rules.yaml) — it knows
# nothing about matching, validation policy, or wire framing beyond the
# message vocabulary it reuses.
add_library(aegis_exchange_order_book
  book.cpp
  book.hpp
  instrument.cpp
  instrument.hpp
  invariants.cpp
  invariants.hpp
  level.hpp
  level_index.cpp
  level_index.hpp
  order_storage.hpp
  types.hpp)

target_include_directories(aegis_exchange_order_book PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_exchange_order_book
  PUBLIC aegis_common aegis_events
  PRIVATE aegis_warnings)
