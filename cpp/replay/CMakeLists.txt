# Layer cpp-replay: the deterministic replay core (M2).
#
# Depends on cpp-common and cpp-events only (configs/architecture_rules.yaml).
# There is deliberately NO edge to any cpp-exchange-* layer: replay must not
# reach into matching or the book, and the exchange must not depend on replay.
# The two are composed at the *process* boundary — aegis_replay emits the
# canonical command form that the existing aegis_exchange_replay CLI already
# consumes — so M2 reuses M1's matching, FIFO priority, cancel/modify semantics
# and sequencing without reimplementing or editing any of it.
#
# Slice 1 populates this layer with the canonical replay record and its total
# order only. The stream reader, virtual clock, pacing modes, fault injection
# and the CLI arrive in slices 9-12.
add_library(aegis_replay
  replay_event.cpp
  replay_event.hpp)

target_include_directories(aegis_replay PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_replay
  PUBLIC aegis_common
  PRIVATE aegis_warnings)
