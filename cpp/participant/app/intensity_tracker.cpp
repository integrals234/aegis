#include "cpp/participant/app/intensity_tracker.hpp"

namespace aegis::participant::app {

void IntensityTracker::observe(const feed::DecodedMessage& message, common::Nanos now_nanos) {
  switch (message.kind) {
    case feed::DecodedKind::kTrade:
      trade_rate_.record_event(now_nanos);
      return;
    case feed::DecodedKind::kOrderTerminated:
      if (message.terminated.has_value() &&
          message.terminated->reason == events::exchange::TerminationReason::kCanceled) {
        cancellation_rate_.record_event(now_nanos);
      }
      return;
    case feed::DecodedKind::kBookSnapshot:
    case feed::DecodedKind::kBookDelta:
    case feed::DecodedKind::kUnhandled:
      return;
  }
}

}  // namespace aegis::participant::app
