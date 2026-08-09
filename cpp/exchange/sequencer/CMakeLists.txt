# Layer cpp-exchange-sequencer: the sole ordering authority. May depend on
# cpp-common and cpp-events only (configs/architecture_rules.yaml) — this is
# what forces the journal to be expressed in envelope terms rather than book
# terms; the sequencer cannot know what an OrderId or a PriceLevel is.
add_library(aegis_exchange_sequencer
  sequencer.cpp
  sequencer.hpp)

target_include_directories(aegis_exchange_sequencer PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_exchange_sequencer
  PUBLIC aegis_common aegis_events
  PRIVATE aegis_warnings)
