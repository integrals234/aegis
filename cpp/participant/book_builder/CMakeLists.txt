# Layer cpp-participant-book-builder (AEGIS-064, AEGIS-065, AEGIS-066,
# AEGIS-067; ADR-0021). May depend on cpp-participant-feed-handler
# (configs/architecture_rules.yaml); not linked here because this slice's
# BookBuilder consumes already-decoded market-data types directly and does
# not yet call into the feed handler itself -- add the edge back if/when it
# does. Not on cpp-exchange-order-book, so it maintains its own level maps.
add_library(aegis_participant_book_builder
  book_builder.cpp
  book_builder.hpp)

target_include_directories(aegis_participant_book_builder PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_participant_book_builder
  PUBLIC aegis_common aegis_events
  PRIVATE aegis_warnings)
