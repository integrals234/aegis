#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/events/exchange_messages.hpp"
#include "cpp/participant/risk/reason_codes.hpp"

/// Plain-old-data vocabulary `RiskEngine` decides over (ADR-0027).
///
/// `cpp-participant-risk.may_depend_on = [cpp-common, cpp-events,
/// cpp-participant-strategy, cpp-statistics]` (`configs/architecture_rules.yaml`)
/// -- no OMS, no portfolio, no book-builder. Every type here is therefore a
/// value the composition root (`cpp/participant/app`) hands in, translated
/// from whatever real state it actually holds (`oms::NewOrderCommand`,
/// `portfolio::Portfolio`, `book::TopOfBook`); `RiskEngine` never reaches into
/// any of those modules itself.
namespace aegis::participant::risk {

using Side = events::exchange::Side;

/// NOLINTNEXTLINE(performance-enum-size)
enum class RiskVerdict : std::uint8_t {
  kApprove = 0,
  kResize = 1,
  kReject = 2,
};

/// One leg's trading intent, before the OMS has assigned a `client_order_id`.
/// `proposal_id`/`leg_index` are assigned by the composition root (the
/// strategy itself emits proposal-only structs with no identifiers, per
/// ADR-0025) and exist so `evaluate_proposal`/`commit_proposal_decision` can
/// reason about every leg of a multi-leg proposal together (AEGIS-137,
/// ADR-0027's atomicity requirement).
struct OrderRequest {
  std::string strategy_id;
  std::string proposal_id;
  std::uint32_t leg_index{0};
  std::uint32_t instrument_id{0};
  Side side{Side::kBuy};
  std::int64_t price_units{0};  ///< The limit/reference price this leg intends to trade at.
  std::int64_t quantity_units{0};
  common::Nanos request_time_nanos{0};  ///< The engine's own clock reading, injected explicitly.
};

/// One leg's outcome, always carrying a `ReasonCode` even on `kApprove`
/// (`kNone`) so a reader never has to distinguish "no reason recorded" from
/// "approved for no binding reason".
struct LegDecision {
  RiskVerdict verdict{RiskVerdict::kReject};
  std::int64_t approved_quantity_units{0};
  ReasonCode reason_code{ReasonCode::kNone};
  std::string reason;
};

/// The ONE canonical terminal decision for a proposal (AEGIS-137): a
/// two-leg calendar spread produces exactly one of these, never two
/// independent objects each claiming to be "the" proposal decision.
/// `legs` carries the per-leg breakdown in the same order as the input,
/// subordinate to `verdict` -- if `verdict == kReject`, every leg's
/// `approved_quantity_units` is 0 regardless of what that leg's own check
/// found, because atomicity means no leg is submitted at all.
struct ProposalDecisionResult {
  RiskVerdict verdict{RiskVerdict::kReject};
  ReasonCode reason_code{ReasonCode::kNone};
  std::string reason;
  std::vector<LegDecision> legs;
};

/// The exact identity a proposal-approved leg is reserved and armed under
/// (M5 closure repair: AEGIS-122/135/137). Deliberately NOT economics
/// (instrument/side/quantity) -- multiple legs, from different proposals or
/// different strategies, may legitimately share identical economics, and an
/// identity built from economics cannot tell them apart (the defect an
/// independent risk-safety review found: a strategy-B order could match a
/// look-alike strategy-A armed leg and inherit A's non-halted state). This
/// triple is the one thing that is guaranteed unique per armed leg: the
/// proposal that armed it, and which leg of that proposal it is.
struct PendingLegKey {
  std::string strategy_id;
  std::string proposal_id;
  std::uint32_t leg_index{0};

  friend bool operator==(const PendingLegKey&, const PendingLegKey&) = default;
};

/// Combines three already-good hashes (`std::hash<std::string>` and
/// `std::hash<std::uint32_t>`) with the boost-style mixing constant, rather
/// than concatenating the fields into one string key -- no allocation per
/// lookup, and no risk of two distinct triples colliding through
/// concatenation ambiguity (e.g. `("a","b",1)` vs `("a","b1",...)`-style
/// string-boundary collisions).
struct PendingLegKeyHash {
  [[nodiscard]] std::size_t operator()(const PendingLegKey& key) const noexcept {
    std::size_t seed = std::hash<std::string>{}(key.strategy_id);
    auto mix = [&seed](std::size_t value) {
      seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    };
    mix(std::hash<std::string>{}(key.proposal_id));
    mix(std::hash<std::uint32_t>{}(key.leg_index));
    return seed;
  }
};

/// A market observation the engine is told about (AEGIS-125, AEGIS-126).
/// `valid` is false for a structurally invalid update (crossed book,
/// non-positive price) -- an invalid update never becomes the new reference
/// price, exactly like a stale one.
struct MarketQuote {
  std::uint32_t instrument_id{0};
  std::int64_t reference_price_units{0};
  common::Nanos observed_at_nanos{0};
  bool valid{true};
};

}  // namespace aegis::participant::risk
