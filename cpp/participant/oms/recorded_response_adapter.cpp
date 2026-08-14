#include "cpp/participant/oms/recorded_response_adapter.hpp"

namespace aegis::participant::oms {

bool RecordedResponseAdapter::submit(const events::exchange::NewOrderCommand& /*command*/) {
  ++cursor_;
  return true;
}

bool RecordedResponseAdapter::cancel(const events::exchange::CancelOrderCommand& /*command*/) {
  ++cursor_;
  return true;
}

bool RecordedResponseAdapter::modify(const events::exchange::ModifyOrderCommand& /*command*/) {
  ++cursor_;
  return true;
}

std::optional<ScriptedResponse> RecordedResponseAdapter::next_response() {
  if (delivered_ >= cursor_ || delivered_ >= script_.size()) {
    return std::nullopt;
  }
  return script_[delivered_++];
}

}  // namespace aegis::participant::oms
