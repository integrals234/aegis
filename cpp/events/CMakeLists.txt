# Layer cpp-events: the versioned message envelope. Depends on cpp-common only
# (configs/architecture_rules.yaml); it carries an opaque payload and therefore
# knows nothing about exchange or participant types.
add_library(aegis_events
  envelope.cpp
  envelope.hpp)

target_include_directories(aegis_events PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_events
  PUBLIC aegis_common
  PRIVATE aegis_warnings)
