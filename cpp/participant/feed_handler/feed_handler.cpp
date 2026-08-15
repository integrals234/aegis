#include "cpp/participant/feed_handler/feed_handler.hpp"

namespace aegis::participant::feed {

DecodedMessage FeedHandler::decode(const events::Envelope& envelope) {
  DecodedMessage result;

  switch (envelope.message_type) {
    case events::MessageType::kBookSnapshot: {
      auto decoded = events::market_data::decode_book_snapshot(envelope.payload);
      if (!decoded.has_value()) {
        return result;  // kUnhandled: malformed payload.
      }
      result.kind = DecodedKind::kBookSnapshot;
      result.sequence_check = trackers_[decoded->instrument_id].observe(decoded->md_sequence);
      result.snapshot = std::move(decoded);
      return result;
    }
    case events::MessageType::kBookDelta: {
      auto decoded = events::market_data::decode_book_delta(envelope.payload);
      if (!decoded.has_value()) {
        return result;
      }
      result.kind = DecodedKind::kBookDelta;
      result.sequence_check = trackers_[decoded->instrument_id].observe(decoded->md_sequence);
      result.delta = decoded;
      return result;
    }
    case events::MessageType::kTrade: {
      auto decoded = events::exchange::decode_trade(envelope.payload);
      if (!decoded.has_value()) {
        return result;
      }
      result.kind = DecodedKind::kTrade;
      result.trade = decoded;
      return result;
    }
    case events::MessageType::kOrderTerminated: {
      auto decoded = events::exchange::decode_order_terminated(envelope.payload);
      if (!decoded.has_value()) {
        return result;
      }
      result.kind = DecodedKind::kOrderTerminated;
      result.terminated = decoded;
      return result;
    }
    default:
      return result;  // kUnhandled.
  }
}

const SequenceTracker* FeedHandler::tracker_for(std::uint32_t instrument_id) const {
  const auto found = trackers_.find(instrument_id);
  return found == trackers_.end() ? nullptr : &found->second;
}

}  // namespace aegis::participant::feed
