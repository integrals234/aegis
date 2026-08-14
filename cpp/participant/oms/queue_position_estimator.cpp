#include "cpp/participant/oms/queue_position_estimator.hpp"

#include <algorithm>
#include <cmath>

namespace aegis::participant::oms {

QueuePositionEstimate QueuePositionEstimator::estimate(std::int64_t observed_volume_ahead_units,
                                                       double assumed_cancellation_rate,
                                                       std::int64_t traded_volume_since_units) {
  QueuePositionEstimate result;
  result.observed_volume_ahead_units = observed_volume_ahead_units;
  result.assumed_cancellation_rate = assumed_cancellation_rate;
  result.effective_volume_ahead_units = static_cast<std::int64_t>(std::llround(
      static_cast<double>(observed_volume_ahead_units) * (1.0 - assumed_cancellation_rate)));

  if (result.effective_volume_ahead_units <= 0) {
    result.fill_probability = 1.0;  // Nothing effectively ahead: already at the front.
    return result;
  }

  const double ratio = static_cast<double>(traded_volume_since_units) /
                       static_cast<double>(result.effective_volume_ahead_units);
  result.fill_probability = std::clamp(ratio, 0.0, 1.0);
  return result;
}

}  // namespace aegis::participant::oms
