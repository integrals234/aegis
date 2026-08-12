#include "cpp/participant/book_builder/book_builder.hpp"

namespace aegis::participant::book {

using events::market_data::BookDeltaEvent;
using events::market_data::BookSnapshotEvent;
using events::market_data::DeltaKind;

void BookBuilder::apply_snapshot(const BookSnapshotEvent& snapshot,
                                 common::Nanos received_at_nanos) {
  bids_.clear();
  asks_.clear();
  orders_.clear();
  last_md_sequence_ = snapshot.md_sequence;
  last_received_nanos_ = received_at_nanos;
  consecutive_faults_ = 0;  // A snapshot is a fresh start, faults or not.

  for (const auto& entry : snapshot.entries) {
    adjust_level(entry.side, entry.price_units, entry.quantity_units);
    if (entry.order_id != 0) {
      orders_[entry.order_id] = OrderView{entry.side, entry.price_units, entry.quantity_units};
    }
  }

  if (recovering_) {
    // AEGIS-070/061: replay every buffered delta the snapshot did not
    // already cover, in the order it was buffered (which is the order it
    // was received, since apply_delta appends).
    std::vector<BookDeltaEvent> to_replay;
    for (const BookDeltaEvent& buffered : buffered_deltas_) {
      if (buffered.md_sequence > snapshot.md_sequence) {
        to_replay.push_back(buffered);
      }
    }
    buffered_deltas_.clear();
    recovering_ = false;  // Before replaying: replayed deltas must apply, not re-buffer.
    for (const BookDeltaEvent& buffered : to_replay) {
      apply_delta(buffered, received_at_nanos);
    }
  }
}

void BookBuilder::apply_delta(const BookDeltaEvent& delta, common::Nanos received_at_nanos) {
  last_received_nanos_ = received_at_nanos;

  if (recovering_) {
    buffered_deltas_.push_back(delta);
    return;
  }

  last_md_sequence_ = delta.md_sequence;

  switch (delta.kind) {
    case DeltaKind::kOrderAdded:
      orders_[delta.order_id] = OrderView{delta.side, delta.price_units, delta.quantity_units};
      adjust_level(delta.side, delta.price_units, delta.quantity_units);
      return;
    case DeltaKind::kOrderModified: {
      const auto found = orders_.find(delta.order_id);
      if (found == orders_.end()) {
        return;  // Unknown order: nothing to modify honestly.
      }
      const std::int64_t change = delta.quantity_units - found->second.quantity_units;
      found->second.quantity_units = delta.quantity_units;
      adjust_level(found->second.side, found->second.price_units, change);
      return;
    }
    case DeltaKind::kOrderRemoved: {
      const auto found = orders_.find(delta.order_id);
      if (found == orders_.end()) {
        return;  // Unknown order: nothing to remove honestly.
      }
      adjust_level(found->second.side, found->second.price_units, -found->second.quantity_units);
      orders_.erase(found);
      return;
    }
    case DeltaKind::kPriceLevelSet:
      set_level(delta.side, delta.price_units, delta.quantity_units);
      return;
  }
}

void BookBuilder::adjust_level(Side side, std::int64_t price_units, std::int64_t delta_units) {
  LevelMap& target = levels_for(side);
  auto [it, inserted] = target.try_emplace(price_units, 0);
  it->second += delta_units;
  if (it->second <= 0) {
    target.erase(it);
  }
}

void BookBuilder::set_level(Side side, std::int64_t price_units, std::int64_t quantity_units) {
  LevelMap& target = levels_for(side);
  if (quantity_units <= 0) {
    target.erase(price_units);
    return;
  }
  target[price_units] = quantity_units;
}

std::optional<PriceLevelView> BookBuilder::best(Side side) const {
  const LevelMap& target = levels_for(side);
  if (target.empty()) {
    return std::nullopt;
  }
  // Bids: best = highest price = last element of an ascending map.
  // Asks: best = lowest price = first element.
  const auto& [price, quantity] = side == Side::kBuy ? *target.rbegin() : *target.begin();
  return PriceLevelView{price, quantity};
}

std::optional<std::int64_t> BookBuilder::quantity_at(Side side, std::int64_t price_units) const {
  const LevelMap& target = levels_for(side);
  const auto found = target.find(price_units);
  return found == target.end() ? std::nullopt : std::optional{found->second};
}

std::vector<PriceLevelView> BookBuilder::levels(Side side, std::size_t depth) const {
  const LevelMap& target = levels_for(side);
  std::vector<PriceLevelView> out;
  out.reserve(depth);
  if (side == Side::kBuy) {
    for (auto it = target.rbegin(); it != target.rend() && out.size() < depth; ++it) {
      out.push_back(PriceLevelView{it->first, it->second});
    }
  } else {
    for (auto it = target.begin(); it != target.end() && out.size() < depth; ++it) {
      out.push_back(PriceLevelView{it->first, it->second});
    }
  }
  return out;
}

std::optional<OrderView> BookBuilder::order(std::uint64_t order_id) const {
  const auto found = orders_.find(order_id);
  return found == orders_.end() ? std::nullopt : std::optional{found->second};
}

void BookBuilder::configure_staleness(common::Duration max_age,
                                      std::uint32_t max_consecutive_faults) {
  max_staleness_age_ = max_age;
  max_consecutive_faults_ = max_consecutive_faults;
}

void BookBuilder::note_message_received(common::Nanos received_at_nanos) {
  last_received_nanos_ = received_at_nanos;
}

void BookBuilder::note_sequence_diagnostic(feed::SequenceDiagnostic diagnostic) {
  if (diagnostic == feed::SequenceDiagnostic::kOk) {
    consecutive_faults_ = 0;
  } else {
    ++consecutive_faults_;
  }
}

bool BookBuilder::is_stale(common::Nanos now_nanos) const {
  if (max_staleness_age_.has_value() && last_received_nanos_.has_value()) {
    const common::Nanos age = now_nanos - *last_received_nanos_;
    if (age > max_staleness_age_->nanos()) {
      return true;
    }
  }
  return max_consecutive_faults_ > 0 && consecutive_faults_ >= max_consecutive_faults_;
}

void BookBuilder::begin_recovery() { recovering_ = true; }

}  // namespace aegis::participant::book
