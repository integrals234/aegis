#include "cpp/exchange/order_book/instrument.hpp"

namespace aegis::exchange {

std::optional<RejectReason> validate_price(const InstrumentSpec& spec, PriceUnits price) {
  if (price < spec.price_floor_units || price > spec.price_ceiling_units) {
    return RejectReason::kPriceOutOfBand;
  }
  const auto offset = price.value() - spec.price_floor_units.value();
  if (offset % spec.tick_size_units != 0) {
    return RejectReason::kPriceNotOnTick;
  }
  return std::nullopt;
}

std::optional<RejectReason> validate_quantity(const InstrumentSpec& spec, QuantityUnits quantity) {
  if (quantity.value() <= 0) {
    return RejectReason::kNonPositiveQuantity;
  }
  if (quantity.value() % spec.lot_size_units != 0) {
    return RejectReason::kQuantityNotOnLot;
  }
  if (quantity < spec.min_quantity_units || quantity > spec.max_quantity_units) {
    return RejectReason::kQuantityOutOfRange;
  }
  return std::nullopt;
}

}  // namespace aegis::exchange
