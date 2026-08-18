#include "cpp/participant/risk/margin_model.hpp"

namespace aegis::participant::risk {
namespace {
[[nodiscard]] constexpr std::int64_t abs64(std::int64_t value) { return value < 0 ? -value : value; }
}  // namespace

std::int64_t required_margin_units(const MarginConfig& config, std::uint32_t instrument_id,
                                   std::int64_t quantity_units) {
  const auto found = config.margin_per_contract_units.find(instrument_id);
  if (found == config.margin_per_contract_units.end()) {
    return 0;
  }
  return found->second * abs64(quantity_units);
}

std::int64_t total_required_margin_units(
    const MarginConfig& config,
    const std::unordered_map<std::uint32_t, std::int64_t>& exposure_by_instrument) {
  std::int64_t total = 0;
  for (const auto& [instrument_id, quantity_units] : exposure_by_instrument) {
    total += required_margin_units(config, instrument_id, quantity_units);
  }
  return total;
}

}  // namespace aegis::participant::risk
