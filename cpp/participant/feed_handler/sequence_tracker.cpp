#include "cpp/participant/feed_handler/sequence_tracker.hpp"

namespace aegis::participant::feed {

SequenceCheckResult SequenceTracker::observe(std::uint64_t sequence) {
  SequenceCheckResult result;
  result.observed_sequence = sequence;

  if (!last_.has_value()) {
    result.diagnostic = SequenceDiagnostic::kOk;
    result.expected_sequence = sequence;
    last_ = sequence;
    consecutive_faults_ = 0;
    return result;
  }

  const std::uint64_t expected = *last_ + 1;
  result.expected_sequence = expected;

  if (sequence == expected) {
    result.diagnostic = SequenceDiagnostic::kOk;
    consecutive_faults_ = 0;
  } else if (sequence == *last_) {
    result.diagnostic = SequenceDiagnostic::kDuplicate;
    ++consecutive_faults_;
  } else if (sequence < *last_) {
    result.diagnostic = SequenceDiagnostic::kReset;
    ++consecutive_faults_;
  } else {
    result.diagnostic = SequenceDiagnostic::kGap;
    ++consecutive_faults_;
  }

  last_ = sequence;
  return result;
}

void SequenceTracker::reset() {
  last_.reset();
  consecutive_faults_ = 0;
}

}  // namespace aegis::participant::feed
