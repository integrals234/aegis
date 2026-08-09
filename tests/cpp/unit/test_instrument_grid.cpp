#include <gtest/gtest.h>

#include "cpp/exchange/order_book/instrument.hpp"

namespace {

using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;
using aegis::exchange::RejectReason;
using aegis::exchange::validate_price;
using aegis::exchange::validate_quantity;

// tick_size_units and lot_size_units are both > 1, so a naive
// price-already-in-ticks or quantity-already-in-lots representation would
// make these tests vacuous (ADR-0009 correction 3).
InstrumentSpec make_spec() {
  return InstrumentSpec{
      .instrument_id = InstrumentId{1},
      .price_floor_units = PriceUnits{1000},
      .price_ceiling_units = PriceUnits{2000},
      .tick_size_units = 25,
      .min_quantity_units = QuantityUnits{100},
      .max_quantity_units = QuantityUnits{10000},
      .lot_size_units = 50,
  };
}

TEST(InstrumentGrid, OnTickOnLotPriceAndQuantityAreAccepted) {
  const auto spec = make_spec();
  EXPECT_FALSE(validate_price(spec, PriceUnits{1050}).has_value());
  EXPECT_FALSE(validate_quantity(spec, QuantityUnits{500}).has_value());
}

TEST(InstrumentGrid, PriceBelowFloorIsOutOfBand) {
  const auto spec = make_spec();
  const auto reason = validate_price(spec, PriceUnits{999});
  ASSERT_TRUE(reason.has_value());
  EXPECT_EQ(reason, RejectReason::kPriceOutOfBand);
}

TEST(InstrumentGrid, PriceAboveCeilingIsOutOfBand) {
  const auto spec = make_spec();
  const auto reason = validate_price(spec, PriceUnits{2001});
  ASSERT_TRUE(reason.has_value());
  EXPECT_EQ(reason, RejectReason::kPriceOutOfBand);
}

TEST(InstrumentGrid, PriceOffTickWithinBandIsRejected) {
  const auto spec = make_spec();
  // 1010 is within [1000, 2000] but (1010 - 1000) % 25 != 0.
  const auto reason = validate_price(spec, PriceUnits{1010});
  ASSERT_TRUE(reason.has_value());
  EXPECT_EQ(reason, RejectReason::kPriceNotOnTick);
}

TEST(InstrumentGrid, OutOfBandIsCheckedBeforeOffTick) {
  const auto spec = make_spec();
  // 2010 is both out of band and off the tick grid; out-of-band must win.
  const auto reason = validate_price(spec, PriceUnits{2010});
  ASSERT_TRUE(reason.has_value());
  EXPECT_EQ(reason, RejectReason::kPriceOutOfBand);
}

TEST(InstrumentGrid, NonPositiveQuantityIsRejectedBeforeLotCheck) {
  const auto spec = make_spec();
  const auto reason = validate_quantity(spec, QuantityUnits{0});
  ASSERT_TRUE(reason.has_value());
  EXPECT_EQ(reason, RejectReason::kNonPositiveQuantity);
}

TEST(InstrumentGrid, QuantityOffLotWithinRangeIsRejected) {
  const auto spec = make_spec();
  // 130 is within [100, 10000] but not a multiple of 50.
  const auto reason = validate_quantity(spec, QuantityUnits{130});
  ASSERT_TRUE(reason.has_value());
  EXPECT_EQ(reason, RejectReason::kQuantityNotOnLot);
}

TEST(InstrumentGrid, OnLotQuantityBelowMinimumIsOutOfRange) {
  const auto spec = make_spec();
  // 50 is a lot multiple but below min_quantity_units (100).
  const auto reason = validate_quantity(spec, QuantityUnits{50});
  ASSERT_TRUE(reason.has_value());
  EXPECT_EQ(reason, RejectReason::kQuantityOutOfRange);
}

TEST(InstrumentGrid, OnLotQuantityAboveMaximumIsOutOfRange) {
  const auto spec = make_spec();
  const auto reason = validate_quantity(spec, QuantityUnits{10050});
  ASSERT_TRUE(reason.has_value());
  EXPECT_EQ(reason, RejectReason::kQuantityOutOfRange);
}

TEST(InstrumentGrid, PriceDomainSizeIsFiniteAndDocumented) {
  const auto spec = make_spec();
  // (2000 - 1000) / 25 + 1 = 41 distinct on-grid prices.
  EXPECT_EQ(spec.price_domain_size(), 41);
}

}  // namespace
