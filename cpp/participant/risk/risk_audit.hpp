#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/participant/risk/risk_types.hpp"

/// The risk decision audit trail (AEGIS-137).
///
/// Two record kinds, deliberately not interchangeable: `ProposalRiskDecision`
/// is the ONE canonical terminal verdict for a proposal_id (ADR-0027) --
/// approve, resize or reject, decided before any leg reaches the OMS.
/// `OrderRiskDecision` is subordinate: one per order that actually reached
/// the mandatory OMS seam, recording what the seam did with the quantity the
/// proposal decision already approved for that leg. A proposal_id therefore
/// has exactly one `ProposalRiskDecision` and, if approved or resized, one
/// `OrderRiskDecision` per leg that reached the seam -- never the reverse
/// relationship, and never two objects both claiming to be "the" decision.
namespace aegis::participant::risk {

struct ProposalRiskDecision {
  std::uint64_t sequence{0};
  common::Nanos decided_at_nanos{0};
  std::string strategy_id;
  std::string proposal_id;
  RiskVerdict verdict{RiskVerdict::kReject};
  ReasonCode reason_code{ReasonCode::kNone};
  std::string reason;
  std::vector<LegDecision> legs;
};

struct OrderRiskDecision {
  std::uint64_t sequence{0};
  common::Nanos decided_at_nanos{0};
  std::string proposal_id;
  std::uint32_t leg_index{0};
  std::uint64_t client_order_id{0};
  std::uint32_t instrument_id{0};
  RiskVerdict verdict{RiskVerdict::kReject};
  std::int64_t requested_quantity_units{0};
  std::int64_t approved_quantity_units{0};
  ReasonCode reason_code{ReasonCode::kNone};
  std::string reason;
};

/// Append-only. Nothing in this class removes or mutates a past record --
/// an audit trail a caller could edit after the fact would prove nothing.
class RiskAuditLog {
 public:
  const ProposalRiskDecision& record_proposal(std::string strategy_id, std::string proposal_id,
                                              RiskVerdict verdict, ReasonCode reason_code,
                                              std::string reason, std::vector<LegDecision> legs,
                                              common::Nanos decided_at_nanos);

  const OrderRiskDecision& record_order(std::string proposal_id, std::uint32_t leg_index,
                                        std::uint64_t client_order_id, std::uint32_t instrument_id,
                                        RiskVerdict verdict, std::int64_t requested_quantity_units,
                                        std::int64_t approved_quantity_units,
                                        ReasonCode reason_code, std::string reason,
                                        common::Nanos decided_at_nanos);

  [[nodiscard]] const std::vector<ProposalRiskDecision>& proposal_decisions() const {
    return proposal_decisions_;
  }
  [[nodiscard]] const std::vector<OrderRiskDecision>& order_decisions() const {
    return order_decisions_;
  }

  /// Test/report helper: how many terminal proposal decisions exist for
  /// `proposal_id` -- the AEGIS-137 invariant asserts this is always 1.
  [[nodiscard]] std::size_t proposal_decision_count(const std::string& proposal_id) const;

 private:
  std::vector<ProposalRiskDecision> proposal_decisions_;
  std::vector<OrderRiskDecision> order_decisions_;
  std::uint64_t next_sequence_{1};
};

}  // namespace aegis::participant::risk
