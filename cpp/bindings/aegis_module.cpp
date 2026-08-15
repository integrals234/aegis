#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "cpp/common/time.hpp"
#include "cpp/common/version.hpp"
#include "cpp/events/envelope.hpp"
#include "cpp/replay/replay_event.hpp"
#include "cpp/statistics/drawdown_tracker.hpp"
#include "cpp/statistics/exponential_stats.hpp"
#include "cpp/statistics/realized_volatility.hpp"
#include "cpp/statistics/rolling_covariance.hpp"
#include "cpp/statistics/rolling_moments.hpp"
#include "cpp/statistics/rolling_zscore.hpp"

/// pybind11 bindings for selected engine APIs (AEGIS-229).
///
/// Deliberately thin. ADR-0005 fixes the binding policy: no binding may mutate
/// engine internals or run on the latency-critical path, and stateful
/// operations required for replay, experiments and paper trading must go
/// through explicit versioned command interfaces routed via the same risk and
/// OMS boundaries as everything else. A binding that hands Python a mutable
/// pointer into a book would make the strategy/risk/OMS path optional from the
/// Python side, which is precisely what AEGIS-120 forbids.
///
/// What is exposed here is what M0 actually has:
///
/// * `version()` and `build_info()` — the build's own description, so a
///   research artifact can record the binary that produced it;
/// * `encode_envelope()` / `decode_envelope()` — pure functions over the
///   canonical wire format, which let a test compare the C++ and Python
///   encoders directly rather than trusting that both match a golden file.
/// * `sort_canonical()` (M2 slice 13) — sorts dicts shaped like
///   `replay::ReplayEvent` into the real canonical replay order using the
///   real `replay::canonical_less`, so a Python caller can prove its own
///   sort agrees with the C++ engine's rather than assuming it does.
/// * the `*_batch` functions (M3 slice 4, AEGIS-107) — each runs one
///   `cpp-statistics` estimator over a full input sequence and returns its
///   trajectory (one output per input), so `tests/unit/test_online_stats_cross_language.py`
///   can compare every intermediate value against `python/common/online_stats.py`,
///   not only the final one. Batch functions, not a stateful class binding:
///   the estimators are exercised through the same `push` sequence a
///   production caller would use, just driven from Python for comparison —
///   nothing here exposes a mutable handle into engine state (ADR-0005).
///
/// Config and metrics remain unbound; nothing here mutates replay state or
/// runs the pacing/fault-injection state machine — the non-statistics
/// functions are pure over plain data, not a binding of the engine itself.
namespace py = pybind11;

namespace {

py::bytes encode_envelope(std::uint64_t sequence, std::uint64_t stream_id,
                          std::int64_t event_time_ns, const std::string& producer_id,
                          const std::string& experiment_id, const std::string& correlation_id,
                          const py::bytes& payload) {
  aegis::events::Envelope envelope;
  envelope.sequence = sequence;
  envelope.stream_id = stream_id;
  envelope.event_time = aegis::common::EventTime{event_time_ns};
  envelope.producer_id = producer_id;
  envelope.experiment_id = experiment_id;
  envelope.correlation_id = correlation_id;

  const std::string raw = payload;
  envelope.payload.reserve(raw.size());
  for (const char character : raw) {
    envelope.payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }

  // std::byte and char have the same size and representation; std::bit_cast on
  // the pointer is the sanctioned spelling of that fact, and it keeps the
  // no-reinterpret_cast rule intact for the places where it protects something.
  const auto encoded = aegis::events::encode(envelope);
  std::string out;
  out.reserve(encoded.size());
  for (const auto byte : encoded) {
    out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return {out};
}

py::dict decode_envelope(const py::bytes& data) {
  const std::string raw = data;
  std::vector<std::byte> bytes;
  bytes.reserve(raw.size());
  for (const char character : raw) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }

  const auto decoded = aegis::events::decode(bytes);
  if (!decoded.has_value()) {
    // Surfaced as a Python exception naming the reason: a feed handler must be
    // able to count *which* kind of malformed message it saw.
    throw py::value_error(std::string{aegis::events::describe(decoded.error())});
  }

  const auto& envelope = decoded.value();
  py::dict result;
  result["schema_version"] = envelope.schema_version;
  result["message_type"] = static_cast<std::uint16_t>(envelope.message_type);
  result["sequence"] = envelope.sequence;
  result["stream_id"] = envelope.stream_id;
  result["event_time_ns"] = envelope.event_time.nanos();
  result["producer_id"] = envelope.producer_id;
  result["experiment_id"] = envelope.experiment_id;
  result["correlation_id"] = envelope.correlation_id;
  std::string payload_out;
  payload_out.reserve(envelope.payload.size());
  for (const auto byte : envelope.payload) {
    payload_out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  result["payload"] = py::bytes(payload_out);
  return result;
}

py::list sort_canonical(const py::list& records) {
  std::vector<aegis::replay::ReplayEvent> events;
  events.reserve(records.size());
  for (const auto& item : records) {
    const auto record = item.cast<py::dict>();
    aegis::replay::ReplayEvent event;
    event.event_time = aegis::common::EventTime{record["event_time_ns"].cast<std::int64_t>()};
    event.source_sequence =
        aegis::replay::SourceSequence{record["source_sequence"].cast<std::uint64_t>()};
    event.contract_symbol = record["contract_symbol"].cast<std::string>();
    event.record_index = aegis::replay::RecordIndex{record["record_index"].cast<std::uint64_t>()};
    events.push_back(std::move(event));
  }

  std::ranges::sort(events, aegis::replay::canonical_less);

  py::list result;
  for (const auto& event : events) {
    py::dict out;
    out["event_time_ns"] = event.event_time.nanos();
    out["source_sequence"] = event.source_sequence.value();
    out["contract_symbol"] = event.contract_symbol;
    out["record_index"] = event.record_index.value();
    result.append(out);
  }
  return result;
}

// -------------------------------------------------------- AEGIS-107 batches

py::dict rolling_moments_batch(const std::vector<double>& values, std::size_t window) {
  aegis::participant::stats::RollingMoments moments(window);
  py::list means;
  py::list variances;
  py::list stddevs;
  for (const double value : values) {
    moments.push(value);
    means.append(moments.mean());
    variances.append(moments.variance());
    stddevs.append(moments.stddev());
  }
  py::dict result;
  result["means"] = means;
  result["variances"] = variances;
  result["stddevs"] = stddevs;
  return result;
}

py::dict rolling_covariance_batch(const std::vector<double>& xs, const std::vector<double>& ys,
                                  std::size_t window) {
  aegis::participant::stats::RollingCovariance covariance(window);
  py::list covariances;
  py::list correlations;
  const std::size_t count = std::min(xs.size(), ys.size());
  for (std::size_t i = 0; i < count; ++i) {
    covariance.push(xs[i], ys[i]);
    covariances.append(covariance.covariance());
    correlations.append(covariance.correlation());
  }
  py::dict result;
  result["covariances"] = covariances;
  result["correlations"] = correlations;
  return result;
}

py::list rolling_zscore_batch(const std::vector<double>& values, std::size_t window) {
  aegis::participant::stats::RollingZScore zscore(window);
  py::list scores;
  for (const double value : values) {
    scores.append(zscore.push_and_score(value));
  }
  return scores;
}

py::dict exponential_stats_batch(const std::vector<double>& values, double alpha) {
  aegis::participant::stats::ExponentialStats stats(alpha);
  py::list means;
  py::list variances;
  for (const double value : values) {
    stats.push(value);
    means.append(stats.mean());
    variances.append(stats.variance());
  }
  py::dict result;
  result["means"] = means;
  result["variances"] = variances;
  return result;
}

py::list realized_volatility_batch(const std::vector<double>& returns, std::size_t window,
                                   double periods_per_year) {
  aegis::participant::stats::RollingRealizedVolatility volatility(window);
  py::list values;
  for (const double value : returns) {
    volatility.push(value);
    values.append(volatility.realized_volatility(periods_per_year));
  }
  return values;
}

py::list rolling_beta_batch(const std::vector<double>& asset_returns,
                            const std::vector<double>& benchmark_returns, std::size_t window) {
  aegis::participant::stats::RollingBeta beta(window);
  py::list values;
  const std::size_t count = std::min(asset_returns.size(), benchmark_returns.size());
  for (std::size_t i = 0; i < count; ++i) {
    beta.push(asset_returns[i], benchmark_returns[i]);
    values.append(beta.beta());
  }
  return values;
}

py::dict drawdown_tracker_batch(const std::vector<double>& values) {
  aegis::participant::stats::DrawdownTracker tracker;
  py::list high_water_marks;
  py::list current_drawdowns;
  py::list max_drawdowns;
  py::list means;
  py::list variances;
  for (const double value : values) {
    tracker.push(value);
    high_water_marks.append(tracker.high_water_mark());
    current_drawdowns.append(tracker.current_drawdown());
    max_drawdowns.append(tracker.max_drawdown());
    means.append(tracker.mean());
    variances.append(tracker.variance());
  }
  py::dict result;
  result["high_water_marks"] = high_water_marks;
  result["current_drawdowns"] = current_drawdowns;
  result["max_drawdowns"] = max_drawdowns;
  result["means"] = means;
  result["variances"] = variances;
  return result;
}

}  // namespace

PYBIND11_MODULE(aegis_bindings, module) {
  module.doc() =
      "Thin bindings over selected AEGIS C++ APIs (AEGIS-229). No binding mutates engine "
      "internals or runs on the latency-critical path; see adr/0005.";

  module.attr("__compiled__") = true;

  module.def("version", &aegis::common::version, "Semantic version of this build.");
  module.def("build_info", &aegis::common::build_info,
             "Compiler, standard, build type, assertion and sanitizer state of this binary.");
  module.def(
      "envelope_schema_version", [] { return aegis::events::kEnvelopeSchemaVersion; },
      "Wire-format version this build encodes and accepts.");

  module.def("encode_envelope", &encode_envelope, py::arg("sequence") = 0, py::arg("stream_id") = 0,
             py::arg("event_time_ns") = 0, py::arg("producer_id") = std::string{},
             py::arg("experiment_id") = std::string{}, py::arg("correlation_id") = std::string{},
             py::arg("payload") = py::bytes(""),
             "Encode an envelope with the C++ encoder and return its canonical bytes.");
  module.def("decode_envelope", &decode_envelope, py::arg("data"),
             "Decode canonical bytes with the C++ decoder; raises ValueError with the reason.");
  module.def("sort_canonical", &sort_canonical, py::arg("records"),
             "Sort dicts shaped like ReplayEvent (event_time_ns, source_sequence, "
             "contract_symbol, record_index) into the real canonical replay order.");

  module.def("rolling_moments_batch", &rolling_moments_batch, py::arg("values"), py::arg("window"),
             "Run RollingMoments over `values`; returns {means, variances, stddevs} "
             "trajectories (AEGIS-098..100, AEGIS-107).");
  module.def("rolling_covariance_batch", &rolling_covariance_batch, py::arg("xs"), py::arg("ys"),
             py::arg("window"),
             "Run RollingCovariance over paired (xs, ys); returns {covariances, correlations} "
             "trajectories (AEGIS-101/102, AEGIS-107).");
  module.def("rolling_zscore_batch", &rolling_zscore_batch, py::arg("values"), py::arg("window"),
             "Run RollingZScore over `values`; returns the leakage-free score trajectory "
             "(AEGIS-103, AEGIS-107).");
  module.def("exponential_stats_batch", &exponential_stats_batch, py::arg("values"),
             py::arg("alpha"),
             "Run ExponentialStats over `values`; returns {means, variances} trajectories "
             "(AEGIS-104, AEGIS-107).");
  module.def("realized_volatility_batch", &realized_volatility_batch, py::arg("returns"),
             py::arg("window"), py::arg("periods_per_year") = 1.0,
             "Run RollingRealizedVolatility over `returns`; returns the volatility trajectory "
             "(AEGIS-105, AEGIS-107).");
  module.def("rolling_beta_batch", &rolling_beta_batch, py::arg("asset_returns"),
             py::arg("benchmark_returns"), py::arg("window"),
             "Run RollingBeta over paired returns; returns the beta trajectory "
             "(AEGIS-105, AEGIS-107).");
  module.def("drawdown_tracker_batch", &drawdown_tracker_batch, py::arg("values"),
             "Run DrawdownTracker over `values`; returns {high_water_marks, current_drawdowns, "
             "max_drawdowns, means, variances} trajectories (AEGIS-106, AEGIS-107).");
}
