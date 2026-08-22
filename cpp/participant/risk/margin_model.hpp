#pragma once

#include <cstdint>
#include <unordered_map>

#include "cpp/participant/risk/risk_limits.hpp"

/// AEGIS-129 (ADR-0028 Model A): a deliberately simplified initial-margin
/// model, documented as NOT SPAN, NOT an exchange clearing model, and NOT a
/// claim of production margin adequacy (`docs/LIMITATIONS.md`).
namespace aegis::participant::risk {

/// `margin_per_contract_units * abs(quantity_units)`. An instrument absent
/// from `config.margin_per_contract_units` requires zero margin -- silently
/// permissive by omission is a config-authoring concern the fixtures/limits
/// tests cover, not a runtime ambiguity this function resolves on its own.
[[nodiscard]] std::int64_t required_margin_units(const MarginConfig& config,
                                                 std::uint32_t instrument_id,
                                                 std::int64_t quantity_units);

/// Sum of `required_margin_units` over every `(instrument_id, quantity_units)`
/// pair in `exposure_by_instrument` -- the caller's combined "position plus
/// every outstanding reservation" view, not owned by this module.
[[nodiscard]] std::int64_t total_required_margin_units(
    const MarginConfig& config,
    const std::unordered_map<std::uint32_t, std::int64_t>& exposure_by_instrument);

}  // namespace aegis::participant::risk
