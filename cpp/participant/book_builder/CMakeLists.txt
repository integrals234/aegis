# Layer cpp-participant-book-builder (AEGIS-064, AEGIS-065, AEGIS-066,
# AEGIS-067, AEGIS-069, AEGIS-070; ADR-0021). note_sequence_diagnostic's
# public signature names feed::SequenceDiagnostic (cpp-participant-feed-handler),
# so that edge is PUBLIC. Not on cpp-exchange-order-book, so it maintains its
# own level maps.
add_library(aegis_participant_book_builder
  book_builder.cpp
  book_builder.hpp)

target_include_directories(aegis_participant_book_builder PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_participant_book_builder
  PUBLIC aegis_common aegis_events aegis_participant_feed_handler
  PRIVATE aegis_warnings)
