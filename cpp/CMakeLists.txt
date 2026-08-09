# AEGIS C++ layers (AEGIS-004, AEGIS-227).
#
# One target per layer in configs/architecture_rules.yaml, named
# aegis_<layer without its language prefix>. tools/check_architecture.py reads
# the target_link_libraries edges below and rejects any that the layer DAG does
# not permit, so the build graph and the architecture cannot drift apart.
#
# Layers that belong to later milestones (participant, replay, memory, queues,
# statistics, and the exchange's own market-data layer, re-dated to M3 by
# ADR-0012) are deliberately absent: the roadmap places them in M2-M8, and an
# empty placeholder target would let their rules pass vacuously.

# nlohmann/json is pinned by release URL and archive hash. It is the only
# third-party runtime dependency of the C++ substrate: the configuration loader
# has to read the same schema file the Python reference loader reads
# (AEGIS-231), and hand-rolling a JSON parser for that would be its own bug farm.
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_subdirectory(common)
add_subdirectory(events)
add_subdirectory(exchange)

if(AEGIS_BUILD_BINDINGS)
  add_subdirectory(bindings)
endif()
