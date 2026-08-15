#include "cpp/exchange/market_data/market_data_publisher.hpp"

#include <optional>

#include "cpp/events/envelope.hpp"
#include "cpp/events/exchange_messages.hpp"

namespace aegis::exchange {
namespace {

using events::MessageType;
using events::market_data::BookDeltaEvent;
using events::market_data::BookLevelEntry;
using events::market_data::BookSnapshotEvent;
using events::market_data::DeltaKind;

void append_side(const OrderBook& book, Side side, BookSnapshotEvent& snapshot) {
  const LevelIndex& index = book.levels(side);
  std::optional<PriceUnits> price = index.best_price();
  while (price.has_value()) {
    for (OrderId const order_id : book.orders_at(side, *price)) {
      const OrderNode* node = book.find(order_id);
      if (node == nullptr) {
        continue;  // Removed between enumeration and lookup: not possible
                   // under single-writer access, guarded defensively.
      }
      BookLevelEntry entry;
      entry.side = side;
      entry.price_units = price->value();
      entry.quantity_units = node->remaining.value();
      entry.order_id = order_id.value();
      snapshot.entries.push_back(entry);
    }
    price = index.next_price_after(*price);
  }
}

}  // namespace

BookSnapshotEvent capture_book_snapshot(const OrderBook& book, std::uint64_t md_sequence) {
  BookSnapshotEvent snapshot;
  snapshot.instrument_id = book.instrument_id().value();
  snapshot.md_sequence = md_sequence;
  append_side(book, Side::kBuy, snapshot);
  append_side(book, Side::kSell, snapshot);
  return snapshot;
}

std::vector<BookDeltaEvent> MarketDataPublisher::observe(
    const std::vector<EmittedMessage>& emitted) {
  std::vector<BookDeltaEvent> deltas;

  for (const EmittedMessage& event : emitted) {
    switch (event.message_type) {
      case MessageType::kOrderAccepted: {
        const auto decoded = events::exchange::decode_order_accepted(event.payload);
        if (!decoded.has_value()) {
          continue;
        }
        tracked_[decoded->order_id] = TrackedOrder{.instrument_id = decoded->instrument_id,
                                                   .side = decoded->side,
                                                   .price_units = decoded->price_units};
        BookDeltaEvent delta;
        delta.instrument_id = decoded->instrument_id;
        delta.md_sequence = next_md_sequence_++;
        delta.kind = DeltaKind::kOrderAdded;
        delta.order_id = decoded->order_id;
        delta.side = decoded->side;
        delta.price_units = decoded->price_units;
        delta.quantity_units = decoded->quantity_units;
        deltas.push_back(delta);
        break;
      }
      case MessageType::kOrderModified: {
        const auto decoded = events::exchange::decode_order_modified(event.payload);
        if (!decoded.has_value()) {
          continue;
        }
        const auto tracked_it = tracked_.find(decoded->order_id);
        if (tracked_it == tracked_.end()) {
          continue;  // Untracked order: nothing this publisher can report.
        }
        BookDeltaEvent delta;
        delta.instrument_id = tracked_it->second.instrument_id;
        delta.md_sequence = next_md_sequence_++;
        delta.kind = DeltaKind::kOrderModified;
        delta.order_id = decoded->order_id;
        delta.side = tracked_it->second.side;
        delta.price_units = tracked_it->second.price_units;
        delta.quantity_units = decoded->new_remaining_units;
        deltas.push_back(delta);
        break;
      }
      case MessageType::kOrderReplaced: {
        const auto decoded = events::exchange::decode_order_replaced(event.payload);
        if (!decoded.has_value()) {
          continue;
        }
        const auto old_it = tracked_.find(decoded->old_order_id);
        if (old_it != tracked_.end()) {
          BookDeltaEvent removed;
          removed.instrument_id = old_it->second.instrument_id;
          removed.md_sequence = next_md_sequence_++;
          removed.kind = DeltaKind::kOrderRemoved;
          removed.order_id = decoded->old_order_id;
          removed.side = old_it->second.side;
          removed.price_units = old_it->second.price_units;
          removed.quantity_units = 0;
          deltas.push_back(removed);
          tracked_.erase(old_it);
        }
        tracked_[decoded->new_order_id] = TrackedOrder{.instrument_id = decoded->instrument_id,
                                                       .side = decoded->side,
                                                       .price_units = decoded->price_units};
        BookDeltaEvent added;
        added.instrument_id = decoded->instrument_id;
        added.md_sequence = next_md_sequence_++;
        added.kind = DeltaKind::kOrderAdded;
        added.order_id = decoded->new_order_id;
        added.side = decoded->side;
        added.price_units = decoded->price_units;
        added.quantity_units = decoded->quantity_units;
        deltas.push_back(added);
        break;
      }
      case MessageType::kOrderTerminated: {
        const auto decoded = events::exchange::decode_order_terminated(event.payload);
        if (!decoded.has_value()) {
          continue;
        }
        const auto tracked_it = tracked_.find(decoded->order_id);
        if (tracked_it == tracked_.end()) {
          continue;  // Already removed via a replace's old-order half.
        }
        BookDeltaEvent delta;
        delta.instrument_id = tracked_it->second.instrument_id;
        delta.md_sequence = next_md_sequence_++;
        delta.kind = DeltaKind::kOrderRemoved;
        delta.order_id = decoded->order_id;
        delta.side = tracked_it->second.side;
        delta.price_units = tracked_it->second.price_units;
        delta.quantity_units = 0;
        deltas.push_back(delta);
        tracked_.erase(tracked_it);
        break;
      }
      default:
        // kOrderRejected, kTrade and anything else cause no book-depth
        // change directly -- the resting-side change (if any) already
        // arrives via its own kOrderModified/kOrderTerminated event.
        break;
    }
  }

  return deltas;
}

}  // namespace aegis::exchange
