#pragma once

#include "cpp/common/time.hpp"
#include "cpp/participant/feed_handler/feed_handler.hpp"
#include "cpp/statistics/rolling_rate.hpp"

/// Trade and cancellation intensity (AEGIS-074; ADR-0020).
///
/// Book/feed-side extraction (recognising which decoded message is a trade
/// or a cancellation) and the generic `cpp-statistics` rate estimator meet
/// only here, in the composition root — neither `cpp-statistics` nor
/// `cpp-participant-feed-handler` depends on the other.
namespace aegis::participant::app {

class IntensityTracker {
 public:
  /// Precondition: `window.nanos() > 0`.
  explicit IntensityTracker(common::Duration window)
      : trade_rate_(window), cancellation_rate_(window) {}

  /// Feeds one `FeedHandler`-decoded message: a `kTrade` records a trade
  /// event; a `kOrderTerminated` whose reason is `kCanceled` records a
  /// cancellation; anything else is a no-op. `now_nanos` is the observation
  /// time — never read from a system clock (the caller's own virtual/manual
  /// clock supplies it).
  void observe(const feed::DecodedMessage& message, common::Nanos now_nanos);

  [[nodiscard]] double trade_rate_per_second() const { return trade_rate_.rate_per_second(); }
  [[nodiscard]] double cancellation_rate_per_second() const {
    return cancellation_rate_.rate_per_second();
  }
  [[nodiscard]] std::size_t trade_count() const { return trade_rate_.count(); }
  [[nodiscard]] std::size_t cancellation_count() const { return cancellation_rate_.count(); }

 private:
  stats::RollingRate trade_rate_;
  stats::RollingRate cancellation_rate_;
};

}  // namespace aegis::participant::app
