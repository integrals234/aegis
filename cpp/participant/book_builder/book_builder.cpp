#include "cpp/participant/book_builder/book_builder.hpp"

#include <utility>

namespace aegis::participant::book {

using events::market_data::BookDeltaEvent;
using events::market_data::BookSnapshotEvent;
using events::market_data::DeltaKind;

void BookBuilder::apply_snapshot(const BookSnapshotEvent& snapshot,
                                 common::Nanos received_at_nanos) {
  tick_adverse_selection();

  bids_.clear();
  asks_.clear();
  orders_.clear();
  bid_watermarks_.clear();
  ask_watermarks_.clear();
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
  tick_adverse_selection();
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
    touch_watermark(side, price_units, 0);
  } else {
    touch_watermark(side, price_units, it->second);
  }
}

void BookBuilder::set_level(Side side, std::int64_t price_units, std::int64_t quantity_units) {
  LevelMap& target = levels_for(side);
  if (quantity_units <= 0) {
    target.erase(price_units);
    touch_watermark(side, price_units, 0);
    return;
  }
  target[price_units] = quantity_units;
  touch_watermark(side, price_units, quantity_units);
}

void BookBuilder::touch_watermark(Side side, std::int64_t price_units, std::int64_t new_quantity) {
  LevelMap& watermarks = watermarks_for(side);
  if (new_quantity <= 0) {
    watermarks.erase(price_units);
    return;
  }
  auto [it, inserted] = watermarks.try_emplace(price_units, new_quantity);
  if (!inserted && new_quantity > it->second) {
    it->second = new_quantity;
  }
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

TopOfBook BookBuilder::top_of_book() const {
  TopOfBook result;
  result.best_bid = best(Side::kBuy);
  result.best_ask = best(Side::kSell);
  if (result.best_bid.has_value() && result.best_ask.has_value()) {
    result.spread_units = result.best_ask->price_units - result.best_bid->price_units;
    result.mid_price_units = (static_cast<double>(result.best_bid->price_units) +
                              static_cast<double>(result.best_ask->price_units)) /
                             2.0;
  }
  return result;
}

std::optional<double> BookBuilder::microprice() const {
  const auto bid = best(Side::kBuy);
  const auto ask = best(Side::kSell);
  if (!bid.has_value() || !ask.has_value()) {
    return std::nullopt;
  }
  const double bid_qty = static_cast<double>(bid->quantity_units);
  const double ask_qty = static_cast<double>(ask->quantity_units);
  const double denominator = bid_qty + ask_qty;
  if (denominator <= 0.0) {
    return std::nullopt;
  }
  // Larger size on one side pulls the price toward the *other* side's price:
  // more resting bid quantity signals buying pressure, pulling the microprice
  // up toward the ask.
  return (bid_qty * static_cast<double>(ask->price_units) +
          ask_qty * static_cast<double>(bid->price_units)) /
         denominator;
}

std::optional<double> BookBuilder::depth_imbalance(std::size_t depth) const {
  if (depth == 0) {
    return std::nullopt;
  }
  std::int64_t bid_depth = 0;
  for (const PriceLevelView& level : levels(Side::kBuy, depth)) {
    bid_depth += level.quantity_units;
  }
  std::int64_t ask_depth = 0;
  for (const PriceLevelView& level : levels(Side::kSell, depth)) {
    ask_depth += level.quantity_units;
  }
  const std::int64_t denominator = bid_depth + ask_depth;
  if (denominator <= 0) {
    return std::nullopt;
  }
  return static_cast<double>(bid_depth - ask_depth) / static_cast<double>(denominator);
}

std::optional<QueueDepletionSignal> BookBuilder::queue_depletion(Side side,
                                                                 std::int64_t price_units) const {
  const LevelMap& target = levels_for(side);
  const auto found = target.find(price_units);
  if (found == target.end()) {
    return std::nullopt;
  }
  const LevelMap& watermarks = side == Side::kBuy ? bid_watermarks_ : ask_watermarks_;
  const auto watermark_it = watermarks.find(price_units);
  const std::int64_t original =
      watermark_it == watermarks.end() ? found->second : watermark_it->second;

  QueueDepletionSignal signal;
  signal.original_quantity_units = original;
  signal.current_quantity_units = found->second;
  signal.depletion_ratio =
      original > 0 ? 1.0 - static_cast<double>(found->second) / static_cast<double>(original) : 0.0;
  return signal;
}

void BookBuilder::record_fill_for_adverse_selection(Side fill_side, std::int64_t price_units,
                                                    std::uint32_t window_updates) {
  pending_adverse_selection_.push_back(
      PendingAdverseSelectionFill{fill_side, price_units, window_updates});
}

void BookBuilder::tick_adverse_selection() {
  for (PendingAdverseSelectionFill& pending : pending_adverse_selection_) {
    if (pending.ticks_remaining > 0) {
      --pending.ticks_remaining;
    }
  }
}

std::vector<AdverseSelectionOutcome> BookBuilder::drain_resolved_adverse_selection() {
  std::vector<AdverseSelectionOutcome> resolved;
  std::vector<PendingAdverseSelectionFill> still_pending;
  for (const PendingAdverseSelectionFill& pending : pending_adverse_selection_) {
    if (pending.ticks_remaining > 0) {
      still_pending.push_back(pending);
      continue;
    }
    AdverseSelectionOutcome outcome;
    outcome.fill_side = pending.side;
    outcome.fill_price_units = pending.price_units;
    const auto mark = best(pending.side);
    if (mark.has_value()) {
      outcome.mark_price_units = mark->price_units;
      outcome.adverse = pending.side == Side::kBuy ? mark->price_units < pending.price_units
                                                   : mark->price_units > pending.price_units;
    }
    resolved.push_back(outcome);
  }
  pending_adverse_selection_ = std::move(still_pending);
  return resolved;
}

}  // namespace aegis::participant::book
