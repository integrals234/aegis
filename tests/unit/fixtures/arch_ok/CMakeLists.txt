add_library(aegis_common cpp/common/clock.hpp)
add_library(aegis_events cpp/events/envelope.hpp)
target_link_libraries(aegis_events PUBLIC aegis_common)
