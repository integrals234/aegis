#include "cpp/participant/risk/risk_audit.hpp"

#include <algorithm>
#include <utility>

namespace aegis::participant::risk {

const ProposalRiskDecision& RiskAuditLog::record_proposal(
    std::string strategy_id, std::string proposal_id, RiskVerdict verdict, ReasonCode reason_code,
    std::string reason, std::vector<LegDecision> legs, common::Nanos decided_at_nanos) {
  // The caller (RiskEngine::commit_proposal_decision) is responsible for the
  // AEGIS-137 replay guard -- this method's own contract is simply "append
  // one record and index it", so the invariant holds regardless of caller
  // discipline as long as no caller ever calls this twice for the same
  // proposal_id (which the guard above ensures).
  const std::string proposal_id_key = proposal_id;
  proposal_decisions_.push_back(ProposalRiskDecision{
      .sequence = next_sequence_++,
      .decided_at_nanos = decided_at_nanos,
      .strategy_id = std::move(strategy_id),
      .proposal_id = std::move(proposal_id),
      .verdict = verdict,
      .reason_code = reason_code,
      .reason = std::move(reason),
      .legs = std::move(legs),
  });
  proposal_index_by_id_[proposal_id_key] = proposal_decisions_.size() - 1;
  return proposal_decisions_.back();
}

const OrderRiskDecision& RiskAuditLog::record_order(
    std::string proposal_id, std::uint32_t leg_index, std::uint64_t client_order_id,
    std::uint32_t instrument_id, RiskVerdict verdict, std::int64_t requested_quantity_units,
    std::int64_t approved_quantity_units, ReasonCode reason_code, std::string reason,
    common::Nanos decided_at_nanos) {
  order_decisions_.push_back(OrderRiskDecision{
      .sequence = next_sequence_++,
      .decided_at_nanos = decided_at_nanos,
      .proposal_id = std::move(proposal_id),
      .leg_index = leg_index,
      .client_order_id = client_order_id,
      .instrument_id = instrument_id,
      .verdict = verdict,
      .requested_quantity_units = requested_quantity_units,
      .approved_quantity_units = approved_quantity_units,
      .reason_code = reason_code,
      .reason = std::move(reason),
  });
  return order_decisions_.back();
}

const ProposalReleaseRiskDecision& RiskAuditLog::record_proposal_release(
    std::string strategy_id, std::string proposal_id, bool authorized, ReasonCode reason_code,
    std::string reason, common::Nanos decided_at_nanos) {
  proposal_release_decisions_.push_back(ProposalReleaseRiskDecision{
      .sequence = next_sequence_++,
      .decided_at_nanos = decided_at_nanos,
      .strategy_id = std::move(strategy_id),
      .proposal_id = std::move(proposal_id),
      .authorized = authorized,
      .reason_code = reason_code,
      .reason = std::move(reason),
  });
  return proposal_release_decisions_.back();
}

std::size_t RiskAuditLog::proposal_release_decision_count(const std::string& proposal_id) const {
  return static_cast<std::size_t>(std::ranges::count_if(
      proposal_release_decisions_, [&proposal_id](const ProposalReleaseRiskDecision& decision) {
        return decision.proposal_id == proposal_id;
      }));
}

std::size_t RiskAuditLog::proposal_decision_count(const std::string& proposal_id) const {
  return static_cast<std::size_t>(std::ranges::count_if(
      proposal_decisions_, [&proposal_id](const ProposalRiskDecision& decision) {
        return decision.proposal_id == proposal_id;
      }));
}

const ProposalRiskDecision* RiskAuditLog::find_proposal_decision(
    const std::string& proposal_id) const {
  const auto found = proposal_index_by_id_.find(proposal_id);
  if (found == proposal_index_by_id_.end()) {
    return nullptr;
  }
  return &proposal_decisions_[found->second];
}

}  // namespace aegis::participant::risk
