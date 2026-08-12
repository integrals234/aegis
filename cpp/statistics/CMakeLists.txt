# Layer cpp-statistics (AEGIS-098, AEGIS-099, AEGIS-100; ADR-0020, ADR-0022).
# Depends on cpp-common only -- a generic numeric library, not a participant
# component.
add_library(aegis_statistics
  rolling_moments.cpp
  rolling_moments.hpp)

target_include_directories(aegis_statistics PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_statistics
  PUBLIC aegis_common
  PRIVATE aegis_warnings)
