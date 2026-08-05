add_library(aegis_common cpp/common/clock.hpp)
add_library(aegis_participant_strategy cpp/participant/strategy/rogue.hpp)
# Violation 8: a link edge no include edge would justify.
target_link_libraries(aegis_common PUBLIC aegis_participant_strategy)
