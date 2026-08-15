#include "cpp/statistics/rolling_zscore.hpp"

namespace aegis::participant::stats {

double RollingZScore::push_and_score(double value) {
  const double prior_mean = moments_.mean();
  const double prior_stddev = moments_.stddev();
  const double score = prior_stddev > 0.0 ? (value - prior_mean) / prior_stddev : 0.0;
  moments_.push(value);
  return score;
}

}  // namespace aegis::participant::stats
