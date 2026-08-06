#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "cpp/common/time.hpp"
#include "cpp/common/version.hpp"
#include "cpp/events/envelope.hpp"

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
///
/// Config, metrics and the replay surfaces are M2 work; the roadmap places
/// "Python bindings needed for data/replay" there, and binding an engine that
/// does not exist yet would be building M1 early.
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
}
