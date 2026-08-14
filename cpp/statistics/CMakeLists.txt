# Layer cpp-statistics (AEGIS-098..107; ADR-0020, ADR-0022). Depends on
# cpp-common only -- a generic numeric library, not a participant component.
add_library(aegis_statistics
  drawdown_tracker.cpp
  drawdown_tracker.hpp
  exponential_stats.cpp
  exponential_stats.hpp
  realized_volatility.cpp
  realized_volatility.hpp
  rolling_covariance.cpp
  rolling_covariance.hpp
  rolling_moments.cpp
  rolling_moments.hpp
  rolling_rate.cpp
  rolling_rate.hpp
  rolling_zscore.cpp
  rolling_zscore.hpp)

target_include_directories(aegis_statistics PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_statistics
  PUBLIC aegis_common
  PRIVATE aegis_warnings)
