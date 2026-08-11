#include "cpp/replay/pacing.hpp"

#include <stdexcept>

namespace aegis::replay {

common::Duration OriginalSpeedPacing::wait_before(const ReplayEvent& previous,
                                                  const ReplayEvent& current) const {
  return current.event_time - previous.event_time;
}

AcceleratedPacing::AcceleratedPacing(double multiplier) : multiplier_(multiplier) {
  if (!(multiplier > 0.0)) {
    throw std::invalid_argument("AcceleratedPacing: multiplier must be strictly positive");
  }
}

common::Duration AcceleratedPacing::wait_before(const ReplayEvent& previous,
                                                const ReplayEvent& current) const {
  const auto original_gap = current.event_time - previous.event_time;
  const auto scaled_nanos =
      static_cast<common::Nanos>(static_cast<double>(original_gap.nanos()) / multiplier_);
  return common::Duration{scaled_nanos};
}

FixedRatePacing::FixedRatePacing(common::Duration interval) : interval_(interval) {
  if (interval_.nanos() < 0) {
    throw std::invalid_argument("FixedRatePacing: interval must be non-negative");
  }
}

common::Duration FixedRatePacing::wait_before(const ReplayEvent& /*previous*/,
                                              const ReplayEvent& /*current*/) const {
  return interval_;
}

common::Duration StepPacing::wait_before(const ReplayEvent& /*previous*/,
                                         const ReplayEvent& /*current*/) const {
  return common::Duration{0};
}

}  // namespace aegis::replay
