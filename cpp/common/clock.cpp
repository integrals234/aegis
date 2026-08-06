#include "cpp/common/clock.hpp"

#include <chrono>

#include "cpp/common/time.hpp"

namespace aegis::common {

Nanos SystemWallClock::now_utc() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
}

MonotonicTime SystemSteadyClock::now() const {
  const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
  return MonotonicTime{std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count()};
}

}  // namespace aegis::common
