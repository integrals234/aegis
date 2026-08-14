#include <gtest/gtest.h>

#include "cpp/participant/oms/latency_model.hpp"

/// AEGIS-113: latency attribution reconciles the total path.
namespace {

using aegis::common::Duration;
using aegis::common::ReceiveTime;
using aegis::participant::oms::LatencyAttribution;
using aegis::participant::oms::LatencyConfig;
using aegis::participant::oms::LatencyModel;

TEST(LatencyModel, StageLatenciesMatchTheConfiguredDelaysExactly) {
  const LatencyModel model(LatencyConfig{
      .decision_delay = Duration{100}, .submit_delay = Duration{250}, .ack_delay = Duration{4000}});
  const LatencyAttribution attribution = model.attribute(ReceiveTime{1'000'000});

  EXPECT_EQ(attribution.receive_time.nanos(), 1'000'000);
  EXPECT_EQ(attribution.decision_time.nanos(), 1'000'100);
  EXPECT_EQ(attribution.submit_time.nanos(), 1'000'350);
  EXPECT_EQ(attribution.ack_time.nanos(), 1'004'350);

  EXPECT_EQ(attribution.decision_latency(), Duration{100});
  EXPECT_EQ(attribution.submit_latency(), Duration{250});
  EXPECT_EQ(attribution.ack_latency(), Duration{4000});
  EXPECT_EQ(attribution.total_latency(), Duration{4350});
}

TEST(LatencyModel, AttributionReconcilesTheTotalPath) {
  const LatencyModel model(LatencyConfig{.decision_delay = Duration{1234},
                                         .submit_delay = Duration{5678},
                                         .ack_delay = Duration{910}});
  const LatencyAttribution attribution = model.attribute(ReceiveTime{42});

  EXPECT_TRUE(attribution.reconciles());
  EXPECT_EQ(
      attribution.decision_latency() + attribution.submit_latency() + attribution.ack_latency(),
      attribution.total_latency());
}

TEST(LatencyModel, ZeroConfiguredDelaysStillReconcileAndCollapseEveryStampTogether) {
  const LatencyModel model(LatencyConfig{});
  const LatencyAttribution attribution = model.attribute(ReceiveTime{7});

  EXPECT_TRUE(attribution.reconciles());
  EXPECT_EQ(attribution.decision_time.nanos(), 7);
  EXPECT_EQ(attribution.submit_time.nanos(), 7);
  EXPECT_EQ(attribution.ack_time.nanos(), 7);
  EXPECT_EQ(attribution.total_latency(), Duration{0});
}

TEST(LatencyModel, DifferentReceiveTimesShiftEveryStampButNotTheStageLatencies) {
  const LatencyModel model(LatencyConfig{
      .decision_delay = Duration{10}, .submit_delay = Duration{20}, .ack_delay = Duration{30}});
  const LatencyAttribution early = model.attribute(ReceiveTime{0});
  const LatencyAttribution later = model.attribute(ReceiveTime{500});

  EXPECT_EQ(early.decision_latency(), later.decision_latency());
  EXPECT_EQ(early.submit_latency(), later.submit_latency());
  EXPECT_EQ(early.ack_latency(), later.ack_latency());
  EXPECT_EQ(later.receive_time.nanos() - early.receive_time.nanos(), 500);
  EXPECT_EQ(later.ack_time.nanos() - early.ack_time.nanos(), 500);
}

}  // namespace
