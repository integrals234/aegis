#include <gtest/gtest.h>

#include "cpp/participant/oms/latency_model.hpp"

/// AEGIS-113: latency attribution reconciles the total path across all five
/// stages the requirement names -- feed, decision, gateway, exchange and
/// acknowledgement.
///
/// The M3 closure audit observed that an earlier `reconciles()` summed
/// consecutive differences and compared them to first-minus-last, which
/// telescopes by construction and cannot fail. The assertions here are
/// written so they *can*: each stage latency is checked against the
/// configured duration it is supposed to represent, the attribution is
/// reconciled against an **independently supplied** observed acknowledgement
/// stamp, and one case deliberately feeds an observed stamp that disagrees
/// with the model to confirm the residual is nonzero.
namespace {

using aegis::common::Duration;
using aegis::common::EventTime;
using aegis::participant::oms::LatencyAttribution;
using aegis::participant::oms::LatencyConfig;
using aegis::participant::oms::LatencyModel;

constexpr LatencyConfig kFiveStage{.feed_delay = Duration{40},
                                   .decision_delay = Duration{100},
                                   .gateway_delay = Duration{250},
                                   .exchange_delay = Duration{700},
                                   .ack_delay = Duration{4000}};

TEST(LatencyModel, EachOfTheFiveStagesIsAttributedToItsConfiguredDuration) {
  const LatencyModel model(kFiveStage);
  const LatencyAttribution attribution = model.attribute(EventTime{1'000'000});

  EXPECT_EQ(attribution.feed_latency(), Duration{40});
  EXPECT_EQ(attribution.decision_latency(), Duration{100});
  EXPECT_EQ(attribution.gateway_latency(), Duration{250});
  EXPECT_EQ(attribution.exchange_latency(), Duration{700});
  EXPECT_EQ(attribution.ack_latency(), Duration{4000});
  EXPECT_EQ(attribution.total_latency(), Duration{5090});
}

TEST(LatencyModel, EveryStampAdvancesThroughItsOwnClockDomainInPathOrder) {
  const LatencyModel model(kFiveStage);
  const LatencyAttribution attribution = model.attribute(EventTime{1'000'000});

  EXPECT_EQ(attribution.event_time.nanos(), 1'000'000);
  EXPECT_EQ(attribution.receive_time.nanos(), 1'000'040);
  EXPECT_EQ(attribution.decision_time.nanos(), 1'000'140);
  EXPECT_EQ(attribution.submit_time.nanos(), 1'000'390);
  EXPECT_EQ(attribution.exchange_time.nanos(), 1'001'090);
  EXPECT_EQ(attribution.ack_time.nanos(), 1'005'090);
}

// The acceptance criterion: attribution reconciles the total path.
TEST(LatencyModel, AttributedStagesAccountForTheWholePathWithNothingLeftOver) {
  const LatencyModel model(kFiveStage);
  const LatencyAttribution attribution = model.attribute(EventTime{42});

  EXPECT_EQ(attribution.attributed_total(), attribution.total_latency());
  EXPECT_EQ(attribution.unattributed_latency(), Duration{0});
  EXPECT_TRUE(attribution.reconciles());
}

// The falsifiable half: reconcile the model against an acknowledgement stamp
// this model did not produce. A path that took longer than modelled leaves a
// real, nonzero residual -- which is the check that could actually fail if
// the model dropped or mis-sized a stage.
TEST(LatencyModel, ResidualAgainstAnIndependentlyObservedAckIsNonZeroWhenTheyDisagree) {
  const LatencyModel model(kFiveStage);
  const LatencyAttribution attribution = model.attribute(EventTime{1'000'000});

  const aegis::common::AckTime observed_on_time{1'005'090};
  EXPECT_EQ(attribution.residual_against(observed_on_time), Duration{0});

  // The exchange took 500ns longer than modelled: the residual must surface
  // it rather than being absorbed silently.
  const aegis::common::AckTime observed_late{1'005'590};
  EXPECT_EQ(attribution.residual_against(observed_late), Duration{500});

  const aegis::common::AckTime observed_early{1'005'000};
  EXPECT_EQ(attribution.residual_against(observed_early), Duration{-90});
}

TEST(LatencyModel, DroppingAStageChangesTheTotalSoAStageCannotBeSilentlyLost) {
  // Exactly kFiveStage but with the gateway leg removed. If gateway latency
  // were not genuinely attributed, these two totals would be equal -- so this
  // is what makes "all five stages are modelled" a falsifiable claim.
  constexpr LatencyConfig kWithoutGateway{.feed_delay = Duration{40},
                                          .decision_delay = Duration{100},
                                          .gateway_delay = Duration{0},
                                          .exchange_delay = Duration{700},
                                          .ack_delay = Duration{4000}};

  const Duration full = LatencyModel(kFiveStage).attribute(EventTime{0}).total_latency();
  const Duration without = LatencyModel(kWithoutGateway).attribute(EventTime{0}).total_latency();

  EXPECT_NE(full, without);
  EXPECT_EQ(Duration{full.nanos() - without.nanos()}, Duration{250});
}

TEST(LatencyModel, ZeroConfiguredDelaysCollapseEveryStampTogetherAndStillReconcile) {
  const LatencyModel model{LatencyConfig{}};
  const LatencyAttribution attribution = model.attribute(EventTime{7});

  EXPECT_TRUE(attribution.reconciles());
  EXPECT_EQ(attribution.total_latency(), Duration{0});
  EXPECT_EQ(attribution.ack_time.nanos(), 7);
}

TEST(LatencyModel, DifferentEventTimesShiftEveryStampButNotTheStageLatencies) {
  const LatencyModel model(kFiveStage);
  const LatencyAttribution early = model.attribute(EventTime{0});
  const LatencyAttribution later = model.attribute(EventTime{500});

  EXPECT_EQ(early.feed_latency(), later.feed_latency());
  EXPECT_EQ(early.exchange_latency(), later.exchange_latency());
  EXPECT_EQ(early.total_latency(), later.total_latency());
  EXPECT_EQ(later.ack_time.nanos() - early.ack_time.nanos(), 500);
}

}  // namespace
