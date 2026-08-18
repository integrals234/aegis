# Layer cpp-participant-strategy (AEGIS-004, AEGIS-076..081; ADR-0025).
# may_depend_on: [cpp-common, cpp-events, cpp-participant-book-builder,
# cpp-statistics] -- no OMS, no exchange, no gateway edge anywhere in this
# target. Populated M4 slice 1 under m4-architecture-transition (PR #10).
add_library(aegis_participant_strategy
  calendar_spread_strategy.cpp
  calendar_spread_strategy.hpp)

target_include_directories(aegis_participant_strategy PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_participant_strategy
  PUBLIC aegis_common aegis_events aegis_participant_book_builder aegis_statistics
  PRIVATE aegis_warnings)
