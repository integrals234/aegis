#include "cpp/participant/app/fault_scenario.hpp"

#include <utility>

#include "cpp/events/envelope.hpp"
#include "cpp/participant/book_builder/book_builder.hpp"
#include "cpp/participant/feed_handler/feed_handler.hpp"
#include "cpp/replay/virtual_clock.hpp"

namespace aegis::participant::app {
namespace {

using events::Envelope;
using events::MessageType;
using events::exchange::Side;
using events::market_data::BookDeltaEvent;
using events::market_data::BookSnapshotEvent;
using events::market_data::encode;
using feed::SequenceDiagnostic;
using replay::DeterministicFaultInjector;
using replay::FaultKind;
using replay::VirtualClock;

Envelope frame_delta(std::uint64_t sequence, std::vector<std::byte> payload) {
  Envelope envelope;
  envelope.message_type = MessageType::kBookDelta;
  envelope.sequence = sequence;
  envelope.stream_id = 1;
  envelope.payload = std::move(payload);
  return envelope;
}

}  // namespace

FaultScenarioOutcome run_fault_scenario(const BookSnapshotEvent& initial_snapshot,
                                        const std::vector<BookDeltaEvent>& deltas,
                                        const std::vector<replay::ReplayEvent>& timing,
                                        const std::vector<replay::FaultRule>& rules,
                                        const std::optional<BookSnapshotEvent>& recovery_snapshot,
                                        common::Duration max_staleness_age,
                                        std::uint32_t max_consecutive_faults) {
  feed::FeedHandler handler;
  book::BookBuilder book(initial_snapshot.instrument_id);
  book.configure_staleness(max_staleness_age, max_consecutive_faults);
  VirtualClock clock;

  book.apply_snapshot(initial_snapshot, clock.now_utc());

  const auto fault_result = DeterministicFaultInjector::apply(timing, rules);

  FaultScenarioOutcome outcome;
  bool recovery_started = false;
  for (const auto& [replay_event, annotation] : fault_result.events) {
    const auto index = replay_event.record_index.value();
    if (index >= deltas.size()) {
      continue;  // Malformed fixture: nothing this scenario can deliver.
    }

    // Checked against the record's *scheduled* time, before any delay is
    // applied: this is what makes a delayed arrival observable as staleness
    // at all -- checking only after the message finally arrives would
    // always see a book "just updated" (age zero), never late (AEGIS-060).
    const common::Nanos scheduled_nanos = replay_event.event_time.nanos();
    if (book.is_stale(scheduled_nanos)) {
      outcome.went_stale = true;
    }

    common::Nanos delivery_nanos = scheduled_nanos;
    if (annotation.has_value() && annotation->kind == FaultKind::kDelayed) {
      delivery_nanos += annotation->delay.nanos();
    }
    clock.advance_to(common::EventTime{delivery_nanos});

    const auto decoded =
        handler.decode(frame_delta(deltas[index].md_sequence, encode(deltas[index])));
    if (!decoded.delta.has_value()) {
      continue;
    }

    book.note_message_received(clock.now_utc());
    if (decoded.sequence_check.has_value()) {
      const SequenceDiagnostic diagnostic = decoded.sequence_check->diagnostic;
      book.note_sequence_diagnostic(diagnostic);

      if (diagnostic == SequenceDiagnostic::kDuplicate) {
        continue;  // Already applied once; re-applying would double-count.
      }
      if (!recovery_started &&
          (diagnostic == SequenceDiagnostic::kGap || diagnostic == SequenceDiagnostic::kReset)) {
        book.begin_recovery();
        recovery_started = true;
      }
    }

    book.apply_delta(*decoded.delta, clock.now_utc());
  }

  if (book.is_stale(clock.now_utc())) {
    outcome.went_stale = true;
  }

  if (recovery_started && recovery_snapshot.has_value()) {
    book.apply_snapshot(*recovery_snapshot, clock.now_utc());
    outcome.recovered = !book.is_recovering();
  }

  if (const auto bid = book.best(Side::kBuy); bid.has_value()) {
    outcome.final_best_bid_price_units = bid->price_units;
    outcome.final_best_bid_quantity_units = bid->quantity_units;
  }
  outcome.final_md_sequence = book.last_md_sequence();
  return outcome;
}

}  // namespace aegis::participant::app
