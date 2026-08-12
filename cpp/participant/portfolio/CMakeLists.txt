# Layer cpp-participant-portfolio (AEGIS-118). May depend on cpp-common and
# cpp-events only -- no edge to the OMS (docs/ARCHITECTURE.md rule 5:
# portfolio state changes only from accepted execution/account events, never
# by reaching into the module that decides them).
add_library(aegis_participant_portfolio
  portfolio.cpp
  portfolio.hpp)

target_include_directories(aegis_participant_portfolio PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aegis_participant_portfolio
  PUBLIC aegis_common aegis_events
  PRIVATE aegis_warnings)
