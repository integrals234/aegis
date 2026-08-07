# Layer cpp-exchange-matching: the FIFO matching policy (AEGIS-040). May
# depend on cpp-common, cpp-events and cpp-exchange-order-book
# (configs/architecture_rules.yaml).
add_library(aegis_exchange_matching
  fifo_policy.cpp
  fifo_policy.hpp
  matching_policy.hpp)

target_include_directories(aegis_exchange_matching PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_exchange_matching
  PUBLIC aegis_common aegis_events aegis_exchange_order_book
  PRIVATE aegis_warnings)
