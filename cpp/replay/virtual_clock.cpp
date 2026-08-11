#include "cpp/replay/virtual_clock.hpp"

#include <stdexcept>
#include <string>

namespace aegis::replay {

void VirtualClock::advance_to(common::EventTime time) {
  if (time.nanos() < now_) {
    throw std::invalid_argument("VirtualClock::advance_to: refusing to move backward from " +
                                std::to_string(now_) + " to " + std::to_string(time.nanos()));
  }
  now_ = time.nanos();
}

}  // namespace aegis::replay
