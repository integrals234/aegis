#include "cpp/participant/oms/latency_model.hpp"

namespace aegis::participant::oms {

LatencyAttribution LatencyModel::attribute(common::EventTime event_time) const {
  // Each stamp is derived from the one before it via `.nanos()`, deliberately
  // rather than through a cross-domain `operator-` -- which `cpp/common/time.hpp`
  // does not provide, and should not (ADR-0002). Bridging the domains is
  // exactly this model's job, so it happens here, once, explicitly.
  const common::ReceiveTime receive_time{event_time.nanos() + config_.feed_delay.nanos()};
  const common::DecisionTime decision_time{receive_time.nanos() + config_.decision_delay.nanos()};
  const common::SubmitTime submit_time{decision_time.nanos() + config_.gateway_delay.nanos()};
  const common::ExchangeTime exchange_time{submit_time.nanos() + config_.exchange_delay.nanos()};
  const common::AckTime ack_time{exchange_time.nanos() + config_.ack_delay.nanos()};
  return LatencyAttribution{.event_time = event_time,
                            .receive_time = receive_time,
                            .decision_time = decision_time,
                            .submit_time = submit_time,
                            .exchange_time = exchange_time,
                            .ack_time = ack_time};
}

}  // namespace aegis::participant::oms
