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
# Slice 1 populated this layer with the canonical replay record and its
# total order only. Slice 9 adds the stream reader, virtual clock,
# deterministic manifest and the replay engine (cursor/resume); slice 10
# adds pacing; slices 11-12 add deterministic fault injection. nlohmann_json
# is a private (implementation-only) dependency, exactly as
# cpp/exchange/app/replay_main.cpp already uses it for its own JSON-Lines
# fixture -- no cpp-replay public header exposes a JSON type.
add_library(aegis_replay
  replay_event.cpp
  replay_event.hpp
  replay_stream.cpp
  replay_stream.hpp
  virtual_clock.cpp
  virtual_clock.hpp
  replay_manifest.cpp
  replay_manifest.hpp
  replay_engine.cpp
  replay_engine.hpp
  pacing.cpp
  pacing.hpp
  fault_injection.cpp
  fault_injection.hpp)

target_include_directories(aegis_replay PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_replay
  PUBLIC aegis_common
  PRIVATE aegis_warnings nlohmann_json::nlohmann_json)

# aegis_replay_run: this layer's CLI entry point (M2 slice 14), the replay-core
# counterpart to aegis_exchange_replay. It exists so "repeated runs produce
# identical outputs" (AEGIS-058) can be shown across separate OS *processes*
# rather than across two calls inside one test binary — the distinction
# tools/determinism_check.py and python/common/determinism.py already treat as
# load-bearing for AEGIS-005, and which applies no less to this core.
add_executable(aegis_replay_run replay_run_main.cpp)
target_link_libraries(aegis_replay_run
  PRIVATE aegis_replay nlohmann_json::nlohmann_json aegis_warnings)
