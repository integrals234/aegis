#include "cpp/participant/oms/latency_model.hpp"

namespace aegis::participant::oms {

LatencyAttribution LatencyModel::attribute(common::ReceiveTime receive_time) const {
  const common::DecisionTime decision_time{receive_time.nanos() + config_.decision_delay.nanos()};
  const common::SubmitTime submit_time{decision_time.nanos() + config_.submit_delay.nanos()};
  const common::AckTime ack_time{submit_time.nanos() + config_.ack_delay.nanos()};
  return LatencyAttribution{.receive_time = receive_time,
                            .decision_time = decision_time,
                            .submit_time = submit_time,
                            .ack_time = ack_time};
}

}  // namespace aegis::participant::oms
