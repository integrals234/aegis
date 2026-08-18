#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/events/market_data_messages.hpp"
#include "cpp/participant/risk/risk_engine.hpp"
#include "cpp/replay/fault_injection.hpp"
#include "cpp/replay/replay_event.hpp"

/// Drives M2's deterministic fault injector through the M3 participant
/// pipeline (AEGIS-060, AEGIS-061; ADR-0021).
///
/// `cpp-participant-app` is the only participant layer permitted to depend on
/// `cpp-replay` (`configs/architecture_rules.yaml`). This is the mechanism
/// ADR-0019 described as missing before M3: "AEGIS-060's stale-data response
/// and AEGIS-061's recovery need a participant-side consumer that does not
/// exist before M3." `replay::DeterministicFaultInjector::apply` itself is
/// consumed unmodified -- this file adds no fault kind and reinterprets
/// nothing about how a fault is declared.
namespace aegis::participant::app {

struct FaultScenarioOutcome {
  /// True if the book was ever judged stale (AEGIS-060) during the run.
  bool went_stale{false};
  /// True if a gap or reset triggered a buffer/re-base/replay recovery
  /// (AEGIS-061/070) and a recovery snapshot was available to complete it.
  bool recovered{false};

  std::optional<std::int64_t> final_best_bid_price_units;
  std::optional<std::int64_t> final_best_bid_quantity_units;
  std::uint64_t final_md_sequence{0};
};

/// `deltas[i]` is the market-data payload for `timing[i]`
/// (`timing[i].record_index.value() == i` by construction of the caller's
/// fixture) -- `cpp-replay`'s `ReplayEvent` carries no payload by design
/// (M2 slice 1), so ordering/timing and content are supplied separately and
/// joined here by `record_index`, the same way `aegis_exchange_replay`
/// joins a canonical command stream to its own ordering.
///
/// `rules` and `timing` are handed to `replay::DeterministicFaultInjector::apply`
/// verbatim. For each surviving (possibly duplicated) record, in the
/// canonical order the injector returns:
///   * `kDelayed` advances the virtual clock by the fault's own `delay`
///     before delivery -- the "stale-data response" is this class judging
///     staleness against that later time, exactly as a real late arrival
///     would age the book (AEGIS-060);
///   * a `kDuplicate` sequence diagnostic is detected and the repeat is not
///     re-applied, so a `kDuplicated` fault cannot double-count a delta;
///   * `kMissing` needs no special handling here: an omitted record is
///     already absent from the injector's output, so the next surviving
///     record's `md_sequence` is what makes `SequenceTracker` report the
///     resulting `kGap` on its own;
///   * a `kGap`/`kReset` diagnostic starts a recovery exactly once; if
///     `recovery_snapshot` is supplied, it completes the recovery
///     (AEGIS-061/070).
[[nodiscard]] FaultScenarioOutcome run_fault_scenario(
    const events::market_data::BookSnapshotEvent& initial_snapshot,
    const std::vector<events::market_data::BookDeltaEvent>& deltas,
    const std::vector<replay::ReplayEvent>& timing, const std::vector<replay::FaultRule>& rules,
    const std::optional<events::market_data::BookSnapshotEvent>& recovery_snapshot,
    common::Duration max_staleness_age, std::uint32_t max_consecutive_faults);

// ---------------------------------------------------------------------------
// AEGIS-062 M5 residual: reproducible RISK RESPONSES to the three market-
// stress fault kinds `cpp/replay/fault_injection.hpp` already delivers
// deterministically (M2 slice 12). This adds no fault kind and reinterprets
// nothing about how `replay::DeterministicFaultInjector::apply` declares or
// orders a fault -- it is the M5 consumer that fault machinery had no risk
// engine to reach before now.
// ---------------------------------------------------------------------------

/// One rule's translation into an actual `risk::RiskEngine` response, so the
/// obligation's "risk responses are reproducible" is a decision the engine
/// really made, not a report computed afterward.
struct MarketStressRiskResponse {
  replay::FaultKind kind{replay::FaultKind::kSpreadWidening};
  std::uint64_t magnitude{0};
  risk::RiskVerdict verdict{risk::RiskVerdict::kReject};
  risk::ReasonCode reason_code{risk::ReasonCode::kNone};
};

/// For each `rules` entry (`kSpreadWidening`, `kVolatilitySpike` or
/// `kLiquidityVanish` -- any other kind is skipped, not silently
/// misinterpreted), deterministically derives the market condition
/// `magnitude` describes and feeds it to `engine` before evaluating one
/// fixed-size test order at `base_price_units`, in the same declared-rule
/// order:
///   * `kSpreadWidening`: `magnitude` basis points of price deviation from
///     the last valid reference -- large enough, this breaches
///     `RiskLimitsConfig::price_collar_bps`;
///   * `kVolatilitySpike`: `magnitude` basis points of alternating return
///     shock pushed through `RiskEngine::on_market_data`, feeding the same
///     `RollingRealizedVolatility` `evaluate` reads (AEGIS-133);
///   * `kLiquidityVanish`: an invalid market update (`on_market_data(...,
///     valid=false)`) -- liquidity vanishing is modelled as the book no
///     longer offering a valid two-sided quote, exactly what AEGIS-126
///     already rejects trading on.
/// `engine` is mutated (each response is a real `evaluate` call, and
/// `on_market_data` is stateful); a caller wanting a clean-slate comparison
/// constructs a fresh engine per call, as every test in
/// `tests/cpp/unit/test_risk_fault_market_stress.cpp` does.
[[nodiscard]] std::vector<MarketStressRiskResponse> run_market_stress_risk_scenario(
    risk::RiskEngine& engine, std::uint32_t instrument_id, std::int64_t base_price_units,
    std::int64_t order_quantity_units, const std::vector<replay::FaultRule>& rules,
    common::Nanos now_nanos);

}  // namespace aegis::participant::app
