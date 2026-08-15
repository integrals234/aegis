#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "cpp/common/time.hpp"
#include "cpp/events/market_data_messages.hpp"
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

}  // namespace aegis::participant::app
