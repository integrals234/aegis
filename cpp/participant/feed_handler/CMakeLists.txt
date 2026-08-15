# Layer cpp-participant-feed-handler (AEGIS-068; ADR-0021). Depends on
# cpp-common and cpp-events only -- no cpp-replay edge.
add_library(aegis_participant_feed_handler
  feed_handler.cpp
  feed_handler.hpp
  sequence_tracker.cpp
  sequence_tracker.hpp)

target_include_directories(aegis_participant_feed_handler PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_participant_feed_handler
  PUBLIC aegis_common aegis_events
  PRIVATE aegis_warnings)
