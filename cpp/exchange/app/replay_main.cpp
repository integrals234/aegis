#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "cpp/exchange/app/exchange_node.hpp"
#include "cpp/exchange/state/snapshot.hpp"

/// `aegis_exchange_replay`: the composition root's CLI entry point (ADR-0012,
/// ADR-0013). Reads a committed JSON-Lines command fixture, applies every
/// command through one `ExchangeNode`, and writes canonical event lines to
/// stdout — the form `python/common/determinism.py`'s `exchange_producer`
/// and the cross-process determinism harness (AEGIS-005) compare byte for
/// byte. `--snapshot-out`/`--restore-from`/`--skip` expose, as a process
/// boundary, the same split-run capability
/// `tests/cpp/unit/test_snapshot_roundtrip.cpp` already exercises in-process.
namespace {

using aegis::common::EventTime;
using aegis::common::ExchangeTime;
using aegis::events::CommandSequence;
using aegis::events::EventSequence;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::exchange::capture_snapshot;
using aegis::exchange::describe;
using aegis::exchange::EmittedEvent;
using aegis::exchange::ExchangeNode;
using aegis::exchange::ExchangeSnapshot;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::OrderBook;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;
using aegis::exchange::read_snapshot;
using aegis::exchange::restore_orders_into;
using aegis::exchange::write_snapshot;
using Json = nlohmann::json;

struct Options {
  std::string input_path;
  std::optional<std::string> snapshot_out;
  std::optional<std::string> restore_from;
  std::uint64_t skip{0};
  std::optional<std::uint64_t> limit;
};

/// One entry per recognized flag, so `parse_args` itself is a flat loop
/// rather than a branch per flag — cognitive-complexity-friendly, and the
/// place to add a flag stays a one-line table entry rather than a new
/// `else if`.
[[nodiscard]] std::unordered_map<std::string, std::function<void(Options&, std::string)>>
make_flag_setters() {
  return {
      {"--input",
       [](Options& options, std::string value) { options.input_path = std::move(value); }},
      {"--snapshot-out",
       [](Options& options, std::string value) { options.snapshot_out = std::move(value); }},
      {"--restore-from",
       [](Options& options, std::string value) { options.restore_from = std::move(value); }},
      {"--skip",
       [](Options& options, const std::string& value) { options.skip = std::stoull(value); }},
      {"--limit",
       [](Options& options, const std::string& value) { options.limit = std::stoull(value); }},
  };
}

[[nodiscard]] std::optional<Options> parse_args(int argc, char** argv) {
  static const auto setters = make_flag_setters();

  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto found = setters.find(flag);
    if (found == setters.end()) {
      std::cerr << "unknown argument: " << flag << "\n";
      return std::nullopt;
    }
    if (i + 1 >= argc) {
      std::cerr << "missing value for " << flag << "\n";
      return std::nullopt;
    }
    ++i;
    found->second(options, argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  if (options.input_path.empty()) {
    std::cerr << "--input PATH is required\n";
    return std::nullopt;
  }
  return options;
}

[[nodiscard]] Side parse_side(const std::string& value) {
  return value == "BUY" ? Side::kBuy : Side::kSell;
}
[[nodiscard]] OrderType parse_order_type(const std::string& value) {
  return value == "LIMIT" ? OrderType::kLimit : OrderType::kMarket;
}

[[nodiscard]] InstrumentSpec parse_instrument(const Json& record) {
  InstrumentSpec spec;
  spec.instrument_id = InstrumentId{record.at("instrument_id").get<std::uint32_t>()};
  spec.price_floor_units = PriceUnits{record.at("price_floor_units").get<std::int64_t>()};
  spec.price_ceiling_units = PriceUnits{record.at("price_ceiling_units").get<std::int64_t>()};
  spec.tick_size_units = record.at("tick_size_units").get<std::int64_t>();
  spec.min_quantity_units = QuantityUnits{record.at("min_quantity_units").get<std::int64_t>()};
  spec.max_quantity_units = QuantityUnits{record.at("max_quantity_units").get<std::int64_t>()};
  spec.lot_size_units = record.at("lot_size_units").get<std::int64_t>();
  return spec;
}

/// Applies one command record to `node`, sequencing it and appending every
/// emitted event to `node`'s own `EventLog` (which the caller reads back via
/// `to_canonical_lines()` once the whole run has finished).
void apply_and_log(ExchangeNode& node, const Json& record, EventTime time) {
  const auto kind = record.at("kind").get<std::string>();
  const auto command_sequence = node.sequencer().sequence(time);
  std::vector<EmittedEvent> emitted;
  if (kind == "new_order") {
    emitted = node.apply_new_order(
        NewOrderCommand{.instrument_id = record.at("instrument_id").get<std::uint32_t>(),
                        .participant_id = record.at("participant_id").get<std::uint64_t>(),
                        .client_order_id = record.at("client_order_id").get<std::uint64_t>(),
                        .side = parse_side(record.at("side").get<std::string>()),
                        .order_type = parse_order_type(record.at("order_type").get<std::string>()),
                        .price_units = record.at("price_units").get<std::int64_t>(),
                        .quantity_units = record.at("quantity_units").get<std::int64_t>()},
        command_sequence);
  } else if (kind == "cancel_order") {
    emitted = node.apply_cancel_order(
        CancelOrderCommand{.instrument_id = record.at("instrument_id").get<std::uint32_t>(),
                           .participant_id = record.at("participant_id").get<std::uint64_t>(),
                           .order_id = record.at("order_id").get<std::uint64_t>()},
        command_sequence);
  } else if (kind == "modify_order") {
    emitted = node.apply_modify_order(
        ModifyOrderCommand{
            .instrument_id = record.at("instrument_id").get<std::uint32_t>(),
            .participant_id = record.at("participant_id").get<std::uint64_t>(),
            .order_id = record.at("order_id").get<std::uint64_t>(),
            .new_price_units = record.at("new_price_units").get<std::int64_t>(),
            .new_quantity_units = record.at("new_quantity_units").get<std::int64_t>()},
        command_sequence);
  } else {
    throw std::runtime_error("unknown command kind: " + kind);
  }
  for (auto& event : emitted) {
    node.event_log().append(event.message_type, std::move(event.payload), time);
  }
}

[[nodiscard]] std::vector<std::byte> read_file_bytes(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  file.seekg(0);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  // A snapshot is opaque binary, not text: reading it through anything but a
  // byte-oriented buffer would risk platform-dependent newline translation.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  file.read(reinterpret_cast<char*>(bytes.data()), size);
  return bytes;
}

void write_file_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path + " for writing");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* data = reinterpret_cast<const char*>(bytes.data());
  file.write(data, static_cast<std::streamsize>(bytes.size()));
}

int run(const Options& options) {
  std::ifstream input(options.input_path);
  if (!input) {
    std::cerr << "cannot open --input " << options.input_path << "\n";
    return 2;
  }

  std::vector<Json> instrument_records;
  std::vector<Json> command_records;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    const auto record = Json::parse(line);
    if (record.at("kind").get<std::string>() == "instrument") {
      instrument_records.push_back(record);
    } else {
      command_records.push_back(record);
    }
  }

  ExchangeSnapshot restored_snapshot;
  const bool restoring = options.restore_from.has_value();
  if (restoring) {
    const auto read_result = read_snapshot(read_file_bytes(*options.restore_from));
    if (!read_result.has_value()) {
      std::cerr << "failed to restore snapshot: " << describe(read_result.error()) << "\n";
      return 2;
    }
    restored_snapshot = read_result.value_or(ExchangeSnapshot{});
  }

  ExchangeNode node = restoring
                          ? ExchangeNode{CommandSequence{restored_snapshot.next_command_sequence},
                                         ExchangeTime{restored_snapshot.last_exchange_time_nanos},
                                         EventSequence{restored_snapshot.next_event_sequence},
                                         restored_snapshot.next_order_id}
                          : ExchangeNode{};

  for (const auto& record : instrument_records) {
    node.add_instrument(parse_instrument(record));
  }
  if (restoring) {
    for (const auto& record : instrument_records) {
      const InstrumentId id{record.at("instrument_id").get<std::uint32_t>()};
      auto* book = node.book(id);
      restore_orders_into(*book, id, restored_snapshot);
    }
  }

  const auto limit = options.limit.value_or(command_records.size());
  std::uint64_t applied = 0;
  for (std::uint64_t i = options.skip; i < command_records.size() && applied < limit;
       ++i, ++applied) {
    const auto& record = command_records[i];
    apply_and_log(node, record, EventTime{record.at("event_time_nanos").get<std::int64_t>()});
  }

  if (options.snapshot_out.has_value()) {
    std::vector<const OrderBook*> books;
    books.reserve(instrument_records.size());
    for (const auto& record : instrument_records) {
      books.push_back(node.book(InstrumentId{record.at("instrument_id").get<std::uint32_t>()}));
    }
    const auto snapshot =
        capture_snapshot(node.sequencer(), node.event_log(), node.next_order_id(), books);
    write_file_bytes(*options.snapshot_out, write_snapshot(snapshot));
  }

  for (const auto& canonical_line : node.event_log().to_canonical_lines()) {
    std::cout << canonical_line << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = parse_args(argc, argv);
  if (!options.has_value()) {
    std::cerr << "usage: aegis_exchange_replay --input PATH [--skip N] [--limit N] "
                 "[--snapshot-out PATH] [--restore-from PATH]\n";
    return 2;
  }
  try {
    return run(*options);
  } catch (const std::exception& error) {
    std::cerr << "aegis_exchange_replay: " << error.what() << "\n";
    return 2;
  }
}
