#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/resource.h>

#include "cpp/common/clock.hpp"
#include "cpp/exchange/app/exchange_node.hpp"

/// `aegis_exchange_bench`: three named, seeded, repeatable workloads
/// (AEGIS-036, AEGIS-039) plus the `docs/BENCHMARK_POLICY.md` required mix
/// (§4.7). `MonotonicTime` appears only here and is never serialized
/// (`cpp/common/time.hpp`'s `serialize_nanos` refuses it at compile time) —
/// this is the one place in the exchange layers a real (not injected) clock
/// is legitimate, because latency measurement is the point.
///
/// **Claim boundary** (stated in every emitted artifact and in
/// `docs/LIMITATIONS.md`): the *asserted* acceptance is operation and
/// allocation counts, which are deterministic. Timing is recorded because the
/// policy requires it to be recorded; every artifact carries
/// `"local_non_comparable": true`, and no latency, throughput, HFT or
/// production claim is derived from any M1 number. Tail-latency measurement
/// and claims belong to M8 (AEGIS-052/053).
namespace {

using aegis::common::elapsed;
using aegis::common::EventTime;
using aegis::common::MonotonicTime;
using aegis::common::SystemSteadyClock;
using aegis::events::exchange::CancelOrderCommand;
using aegis::events::exchange::decode_order_accepted;
using aegis::events::exchange::decode_trade;
using aegis::events::exchange::ModifyOrderCommand;
using aegis::events::exchange::NewOrderCommand;
using aegis::events::exchange::OrderType;
using aegis::events::exchange::Side;
using aegis::exchange::ExchangeNode;
using aegis::exchange::InstrumentId;
using aegis::exchange::InstrumentSpec;
using aegis::exchange::PriceUnits;
using aegis::exchange::QuantityUnits;
using Json = nlohmann::json;

constexpr std::int64_t kLotSizeUnits = 50;
constexpr std::int64_t kTickSizeUnits = 25;
constexpr std::int64_t kPriceFloorUnits = 1000;

/// `int` workload constants (loop counters, so they stay `int`) sized once,
/// explicitly, rather than combined and widened afterward — the widening
/// must happen before any arithmetic that could overflow `int`.
[[nodiscard]] constexpr std::size_t sz(int value) { return static_cast<std::size_t>(value); }

/// Counts allocation *calls*, not bytes: AEGIS-037's claim is about counts,
/// never about latency (ADR-0010), and this workload's own book construction is
/// expected to allocate freely — only the steady-state hot loop's allocation
/// count is asserted to be interesting, and this resource makes that number
/// visible rather than asserted.
class CountingResource final : public std::pmr::memory_resource {
 public:
  [[nodiscard]] std::size_t allocations() const { return allocations_; }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    ++allocations_;
    return std::pmr::get_default_resource()->allocate(bytes, alignment);
  }
  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
    std::pmr::get_default_resource()->deallocate(ptr, bytes, alignment);
  }
  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::size_t allocations_{0};
};

[[nodiscard]] InstrumentSpec make_bench_spec() {
  InstrumentSpec spec;
  spec.instrument_id = InstrumentId{1};
  spec.price_floor_units = PriceUnits{kPriceFloorUnits};
  spec.price_ceiling_units = PriceUnits{kPriceFloorUnits + (kTickSizeUnits * 20000)};
  spec.tick_size_units = kTickSizeUnits;
  spec.min_quantity_units = QuantityUnits{kLotSizeUnits};
  spec.max_quantity_units = QuantityUnits{kLotSizeUnits * 1000};
  spec.lot_size_units = kLotSizeUnits;
  return spec;
}

/// Total user+system CPU time consumed by this process so far, in seconds
/// (`docs/BENCHMARK_POLICY.md` requires CPU utilization alongside
/// throughput). `getrusage` is POSIX and portable to every platform this
/// codebase builds on; unlike `SystemSteadyClock`, it measures work done,
/// not wall time elapsed, so the ratio of the two across one measured run is
/// the utilization figure.
[[nodiscard]] double cpu_seconds_now() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const auto to_seconds = [](const timeval& value) {
    return static_cast<double>(value.tv_sec) + (static_cast<double>(value.tv_usec) / 1e6);
  };
  return to_seconds(usage.ru_utime) + to_seconds(usage.ru_stime);
}

struct FillTracker {
  int fill_count{0};
  std::int64_t filled_quantity_units{0};
};

/// Scans one command's emitted events for trades and folds them into
/// `tracker`. Used by `run_policy_mix`, where "add" never crosses (buy-only
/// additions against a buy-only seeded book) but "marketable" and
/// "multi_level" can — scanning every `apply_new_order` result generically,
/// rather than assuming which branches fill, is what keeps this correct if
/// that ever changes.
void accumulate_fills(FillTracker& tracker,
                      const std::vector<aegis::exchange::EmittedEvent>& emitted) {
  for (const auto& event : emitted) {
    if (event.message_type == aegis::events::MessageType::kTrade) {
      const auto trade =
          decode_trade(event.payload).value_or(aegis::events::exchange::TradeEvent{});
      ++tracker.fill_count;
      tracker.filled_quantity_units += trade.quantity_units;
    }
  }
}

[[nodiscard]] Json summarize_latency(std::vector<double> nanos) {
  std::ranges::sort(nanos);
  const auto percentile = [&](double fraction) -> double {
    if (nanos.empty()) {
      return 0.0;
    }
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(nanos.size() - 1));
    return nanos[index];
  };
  return Json{
      {"count", nanos.size()},        {"median_ns", percentile(0.50)},
      {"p95_ns", percentile(0.95)},   {"p99_ns", percentile(0.99)},
      {"p999_ns", percentile(0.999)}, {"max_ns", nanos.empty() ? 0.0 : nanos.back()},
  };
}

/// Submits one resting limit buy at `price_units` and returns its assigned
/// `OrderId`. Buy-only construction never crosses, so book-building never
/// matches — every level starts genuinely empty and stays exactly as built.
[[nodiscard]] std::uint64_t submit_resting_order(ExchangeNode& node, std::uint64_t& next_client_id,
                                                 std::int64_t& next_event_nanos,
                                                 std::int64_t price_units,
                                                 std::int64_t quantity_units) {
  const auto command_sequence = node.sequencer().sequence(EventTime{next_event_nanos++});
  const auto events = node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                           .participant_id = 1,
                                                           .client_order_id = next_client_id++,
                                                           .side = Side::kBuy,
                                                           .order_type = OrderType::kLimit,
                                                           .price_units = price_units,
                                                           .quantity_units = quantity_units},
                                           command_sequence);
  const auto accepted = decode_order_accepted(events.front().payload)
                            .value_or(aegis::events::exchange::OrderAcceptedEvent{});
  return accepted.order_id;
}

[[nodiscard]] Json make_report(const std::string& workload, std::uint64_t seed,
                               int warmup_operations, int measured_operations,
                               double total_wall_seconds, double cpu_utilization, Json latencies,
                               Json message_mix, int instruments, int levels, int orders,
                               Json fill_distribution, std::size_t allocation_count,
                               const std::string& reproducible_command) {
  return Json{
      {"workload", workload},
      {"seed", seed},
      {"warmup_operations", warmup_operations},
      {"measured_operations", measured_operations},
      {"throughput_ops_per_sec", total_wall_seconds > 0.0
                                     ? static_cast<double>(measured_operations) / total_wall_seconds
                                     : 0.0},
      {"cpu_utilization", cpu_utilization},
      {"latencies", std::move(latencies)},
      {"message_mix", std::move(message_mix)},
      {"instruments", instruments},
      {"levels", levels},
      {"orders", orders},
      {"fill_distribution", std::move(fill_distribution)},
      {"allocation_count", allocation_count},
      {"local_non_comparable", true},
      {"claim",
       "Operation and allocation counts are the asserted acceptance (AEGIS-036/AEGIS-039); "
       "timing and cpu_utilization are recorded because docs/BENCHMARK_POLICY.md requires "
       "them, are local and non-comparable, and are not a latency, throughput, HFT or "
       "production claim."},
      {"reproducible_command", reproducible_command},
  };
}

/// `cpu_utilization = process CPU seconds consumed / wall-clock seconds
/// elapsed` across exactly the bracketed measured run — 1.0 for a
/// single-threaded loop that never blocks, lower if the OS scheduled this
/// process out.
[[nodiscard]] double cpu_utilization_between(double cpu_seconds_before, double cpu_seconds_after,
                                             double wall_seconds) {
  return wall_seconds > 0.0 ? (cpu_seconds_after - cpu_seconds_before) / wall_seconds : 0.0;
}

/// AEGIS-036: N resting orders across `kLevels` price levels, then K seeded
/// lookups and K seeded cancels by `OrderId`.
[[nodiscard]] Json run_lookup_cancel(std::uint64_t seed) {
  constexpr int kLevels = 200;
  constexpr int kOrdersPerLevel = 25;  // N = 5000 resting orders.
  constexpr int kWarmupOps = 500;
  constexpr int kMeasuredOps = 4000;

  CountingResource resource;
  ExchangeNode node;
  const auto spec = make_bench_spec();
  node.add_instrument(spec, (sz(kLevels) * sz(kOrdersPerLevel)) + sz(kMeasuredOps) + sz(kWarmupOps),
                      &resource);

  std::uint64_t next_client_id = 1;
  std::int64_t next_event_nanos = 0;
  std::vector<std::uint64_t> live_ids;
  live_ids.reserve(sz(kLevels) * sz(kOrdersPerLevel));
  for (int level = 0; level < kLevels; ++level) {
    const auto price = kPriceFloorUnits + (level * kTickSizeUnits);
    for (int i = 0; i < kOrdersPerLevel; ++i) {
      live_ids.push_back(
          submit_resting_order(node, next_client_id, next_event_nanos, price, kLotSizeUnits));
    }
  }

  std::mt19937_64 rng{seed};  // NOLINT(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::ranges::shuffle(live_ids, rng);

  const auto* book = node.book(InstrumentId{1});
  const SystemSteadyClock clock;

  const auto run_once = [&](int count, int offset, std::vector<double>* lookup_out,
                            std::vector<double>* cancel_out) {
    for (int i = 0; i < count; ++i) {
      const auto order_id = aegis::exchange::OrderId{live_ids[sz(offset) + sz(i)]};
      const auto lookup_start = clock.now();
      std::ignore = book->find(order_id);
      const auto lookup_end = clock.now();
      if (lookup_out != nullptr) {
        lookup_out->push_back(static_cast<double>(elapsed(lookup_start, lookup_end).nanos()));
      }

      const auto cancel_start = clock.now();
      std::ignore = node.apply_cancel_order(
          CancelOrderCommand{.instrument_id = 1, .participant_id = 1, .order_id = order_id.value()},
          node.sequencer().sequence(EventTime{next_event_nanos++}));
      const auto cancel_end = clock.now();
      if (cancel_out != nullptr) {
        cancel_out->push_back(static_cast<double>(elapsed(cancel_start, cancel_end).nanos()));
      }
    }
  };

  run_once(kWarmupOps, 0, nullptr, nullptr);
  const auto allocations_before_measured = resource.allocations();

  std::vector<double> lookup_nanos;
  std::vector<double> cancel_nanos;
  lookup_nanos.reserve(kMeasuredOps);
  cancel_nanos.reserve(kMeasuredOps);
  const auto cpu_start = cpu_seconds_now();
  const auto wall_start = clock.now();
  run_once(kMeasuredOps, kWarmupOps, &lookup_nanos, &cancel_nanos);
  const auto wall_end = clock.now();
  const auto cpu_end = cpu_seconds_now();
  const auto wall_seconds = elapsed(wall_start, wall_end).seconds();

  return make_report("lookup_cancel", seed, kWarmupOps, kMeasuredOps, wall_seconds,
                     cpu_utilization_between(cpu_start, cpu_end, wall_seconds),
                     Json{{"lookup", summarize_latency(lookup_nanos)},
                          {"cancel", summarize_latency(cancel_nanos)}},
                     Json{{"lookup", 50}, {"cancel", 50}}, 1, kLevels, kLevels * kOrdersPerLevel,
                     Json::object(), resource.allocations() - allocations_before_measured,
                     "build/release/cpp/exchange/app/aegis_exchange_bench --workload lookup_cancel "
                     "--seed " +
                         std::to_string(seed));
}

/// AEGIS-039: every aggressor consumes exactly one resting order. The level
/// it took is refilled identically before the next aggressor, so the book
/// stays in steady state across the whole measured run.
[[nodiscard]] Json run_single_fill_aggressor(std::uint64_t seed) {
  constexpr int kWarmupOps = 200;
  constexpr int kMeasuredOps = 3000;
  constexpr std::int64_t kRestingQuantity = kLotSizeUnits * 2;

  CountingResource resource;
  ExchangeNode node;
  const auto spec = make_bench_spec();
  node.add_instrument(spec, sz(kMeasuredOps) + sz(kWarmupOps) + 8, &resource);

  std::uint64_t next_client_id = 1;
  std::int64_t next_event_nanos = 0;
  const std::int64_t resting_price = kPriceFloorUnits + (kTickSizeUnits * 50);
  std::ignore =
      submit_resting_order(node, next_client_id, next_event_nanos, resting_price, kRestingQuantity);

  const SystemSteadyClock clock;
  const auto run_once = [&](int count, std::vector<double>* out) {
    for (int i = 0; i < count; ++i) {
      const auto start = clock.now();
      const auto events =
          node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                               .participant_id = 2,
                                               .client_order_id = next_client_id++,
                                               .side = Side::kSell,
                                               .order_type = OrderType::kLimit,
                                               .price_units = resting_price,
                                               .quantity_units = kRestingQuantity},
                               node.sequencer().sequence(EventTime{next_event_nanos++}));
      const auto end = clock.now();
      if (out != nullptr) {
        out->push_back(static_cast<double>(elapsed(start, end).nanos()));
      }
      std::ignore = events;
      // Refill the level the aggressor just swept, so the next aggressor
      // sees the same single-fill scenario.
      std::ignore = submit_resting_order(node, next_client_id, next_event_nanos, resting_price,
                                         kRestingQuantity);
    }
  };

  run_once(kWarmupOps, nullptr);
  const auto allocations_before_measured = resource.allocations();
  std::vector<double> nanos;
  nanos.reserve(kMeasuredOps);
  const auto cpu_start = cpu_seconds_now();
  const auto wall_start = clock.now();
  run_once(kMeasuredOps, &nanos);
  const auto wall_end = clock.now();
  const auto cpu_end = cpu_seconds_now();
  const auto wall_seconds = elapsed(wall_start, wall_end).seconds();

  return make_report("single_fill_aggressor", seed, kWarmupOps, kMeasuredOps, wall_seconds,
                     cpu_utilization_between(cpu_start, cpu_end, wall_seconds),
                     Json{{"aggressor", summarize_latency(nanos)}}, Json{{"marketable", 100}}, 1, 1,
                     1, Json{{"single_fill", kMeasuredOps}},
                     resource.allocations() - allocations_before_measured,
                     "build/release/cpp/exchange/app/aegis_exchange_bench "
                     "--workload single_fill_aggressor --seed " +
                         std::to_string(seed));
}

/// AEGIS-039: every aggressor sweeps exactly `k` resting orders across `k`
/// price levels (one resting order per level), refilled identically before
/// the next aggressor — the output-sensitive claim this exercises is that
/// visited-order count tracks `k`, not the size of the rest of the book.
[[nodiscard]] Json run_multi_fill_multi_level_aggressor(std::uint64_t seed, int k) {
  constexpr int kWarmupOps = 50;
  constexpr int kMeasuredOps = 500;
  constexpr std::int64_t kRestingQuantity = kLotSizeUnits;

  CountingResource resource;
  ExchangeNode node;
  const auto spec = make_bench_spec();
  node.add_instrument(spec, ((sz(kMeasuredOps) + sz(kWarmupOps) + 1) * sz(k + 1)) + 8, &resource);

  std::uint64_t next_client_id = 1;
  std::int64_t next_event_nanos = 0;

  const auto refill_levels = [&]() {
    for (int level = 0; level < k; ++level) {
      const auto price = kPriceFloorUnits + (level * kTickSizeUnits);
      std::ignore =
          submit_resting_order(node, next_client_id, next_event_nanos, price, kRestingQuantity);
    }
  };
  refill_levels();

  const std::int64_t sweep_price = kPriceFloorUnits + ((k - 1) * kTickSizeUnits);
  const std::int64_t sweep_quantity = kRestingQuantity * k;

  const SystemSteadyClock clock;
  const auto run_once = [&](int count, std::vector<double>* out) {
    for (int i = 0; i < count; ++i) {
      const auto start = clock.now();
      std::ignore = node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                         .participant_id = 3,
                                                         .client_order_id = next_client_id++,
                                                         .side = Side::kSell,
                                                         .order_type = OrderType::kLimit,
                                                         .price_units = sweep_price,
                                                         .quantity_units = sweep_quantity},
                                         node.sequencer().sequence(EventTime{next_event_nanos++}));
      const auto end = clock.now();
      if (out != nullptr) {
        out->push_back(static_cast<double>(elapsed(start, end).nanos()));
      }
      refill_levels();
    }
  };

  run_once(kWarmupOps, nullptr);
  const auto allocations_before_measured = resource.allocations();
  std::vector<double> nanos;
  nanos.reserve(kMeasuredOps);
  const auto cpu_start = cpu_seconds_now();
  const auto wall_start = clock.now();
  run_once(kMeasuredOps, &nanos);
  const auto wall_end = clock.now();
  const auto cpu_end = cpu_seconds_now();
  const auto wall_seconds = elapsed(wall_start, wall_end).seconds();

  return make_report("multi_fill_multi_level_aggressor_k" + std::to_string(k), seed, kWarmupOps,
                     kMeasuredOps, wall_seconds,
                     cpu_utilization_between(cpu_start, cpu_end, wall_seconds),
                     Json{{"aggressor", summarize_latency(nanos)}}, Json{{"marketable", 100}}, 1, k,
                     k, Json{{"multi_fill", Json{{"k", k}, {"count", kMeasuredOps}}}},
                     resource.allocations() - allocations_before_measured,
                     "build/release/cpp/exchange/app/aegis_exchange_bench "
                     "--workload multi_fill_multi_level_aggressor --k " +
                         std::to_string(k) + " --seed " + std::to_string(seed));
}

/// The `docs/BENCHMARK_POLICY.md` required mix: 45% add, 30% cancel, 10%
/// modify, 10% marketable, 5% large multi-level-crossing orders.
[[nodiscard]] Json run_policy_mix(std::uint64_t seed) {
  constexpr int kWarmupOps = 500;
  constexpr int kMeasuredOps = 5000;
  constexpr int kLevels = 100;

  CountingResource resource;
  ExchangeNode node;
  const auto spec = make_bench_spec();
  node.add_instrument(spec, ((sz(kWarmupOps) + sz(kMeasuredOps)) * 2) + (sz(kLevels) * 10),
                      &resource);

  std::uint64_t next_client_id = 1;
  std::int64_t next_event_nanos = 0;
  std::vector<std::uint64_t> live_ids;

  // Seed a resting book so cancel/modify have something to act on from the
  // first measured operation.
  for (int level = 0; level < kLevels; ++level) {
    const auto price = kPriceFloorUnits + (level * kTickSizeUnits);
    for (int i = 0; i < 5; ++i) {
      live_ids.push_back(
          submit_resting_order(node, next_client_id, next_event_nanos, price, kLotSizeUnits));
    }
  }

  std::mt19937_64 rng{seed};  // NOLINT(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::uniform_int_distribution<int> mix_dist(1, 100);
  std::uniform_int_distribution<std::size_t> level_dist(0, kLevels - 1);

  int add_count = 0;
  int cancel_count = 0;
  int modify_count = 0;
  int marketable_count = 0;
  int multi_level_count = 0;
  FillTracker fill_tracker;

  const auto run_once = [&](int count, std::vector<double>* out) {
    for (int i = 0; i < count; ++i) {
      const auto roll = mix_dist(rng);
      const auto start = SystemSteadyClock{}.now();
      const auto command_sequence = node.sequencer().sequence(EventTime{next_event_nanos++});
      if (roll <= 45) {  // add
        const auto price =
            kPriceFloorUnits + (static_cast<std::int64_t>(level_dist(rng)) * kTickSizeUnits);
        const auto events =
            node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                 .participant_id = 1,
                                                 .client_order_id = next_client_id++,
                                                 .side = Side::kBuy,
                                                 .order_type = OrderType::kLimit,
                                                 .price_units = price,
                                                 .quantity_units = kLotSizeUnits},
                                 command_sequence);
        const auto accepted = decode_order_accepted(events.front().payload)
                                  .value_or(aegis::events::exchange::OrderAcceptedEvent{});
        if (accepted.order_id != 0) {
          live_ids.push_back(accepted.order_id);
        }
        accumulate_fills(fill_tracker, events);
        ++add_count;
      } else if (roll <= 75 && !live_ids.empty()) {  // cancel
        const auto index = level_dist(rng) % live_ids.size();
        std::ignore = node.apply_cancel_order(
            CancelOrderCommand{
                .instrument_id = 1, .participant_id = 1, .order_id = live_ids[index]},
            command_sequence);
        live_ids.erase(live_ids.begin() + static_cast<std::ptrdiff_t>(index));
        ++cancel_count;
      } else if (roll <= 85 &&
                 !live_ids.empty()) {  // modify (quantity decrease, priority-retaining)
        const auto index = level_dist(rng) % live_ids.size();
        std::ignore =
            node.apply_modify_order(ModifyOrderCommand{.instrument_id = 1,
                                                       .participant_id = 1,
                                                       .order_id = live_ids[index],
                                                       .new_price_units = kPriceFloorUnits,
                                                       .new_quantity_units = kLotSizeUnits},
                                    command_sequence);
        ++modify_count;
      } else if (roll <= 95) {  // marketable
        accumulate_fills(fill_tracker,
                         node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                              .participant_id = 4,
                                                              .client_order_id = next_client_id++,
                                                              .side = Side::kSell,
                                                              .order_type = OrderType::kLimit,
                                                              .price_units = kPriceFloorUnits,
                                                              .quantity_units = kLotSizeUnits},
                                              command_sequence));
        ++marketable_count;
      } else {  // large multi-level-crossing order
        accumulate_fills(fill_tracker,
                         node.apply_new_order(NewOrderCommand{.instrument_id = 1,
                                                              .participant_id = 5,
                                                              .client_order_id = next_client_id++,
                                                              .side = Side::kSell,
                                                              .order_type = OrderType::kLimit,
                                                              .price_units = kPriceFloorUnits,
                                                              .quantity_units = kLotSizeUnits * 20},
                                              command_sequence));
        ++multi_level_count;
      }
      const auto end = SystemSteadyClock{}.now();
      if (out != nullptr) {
        out->push_back(static_cast<double>(elapsed(start, end).nanos()));
      }
    }
  };

  run_once(kWarmupOps, nullptr);
  add_count = cancel_count = modify_count = marketable_count = multi_level_count = 0;
  fill_tracker = FillTracker{};
  const auto allocations_before_measured = resource.allocations();
  std::vector<double> nanos;
  nanos.reserve(kMeasuredOps);
  const auto clock = SystemSteadyClock{};
  const auto cpu_start = cpu_seconds_now();
  const auto wall_start = clock.now();
  run_once(kMeasuredOps, &nanos);
  const auto wall_end = clock.now();
  const auto cpu_end = cpu_seconds_now();
  const auto wall_seconds = elapsed(wall_start, wall_end).seconds();

  return make_report(
      "policy_mix", seed, kWarmupOps, kMeasuredOps, wall_seconds,
      cpu_utilization_between(cpu_start, cpu_end, wall_seconds),
      Json{{"operation", summarize_latency(nanos)}},
      Json{{"add", add_count},
           {"cancel", cancel_count},
           {"modify", modify_count},
           {"marketable", marketable_count},
           {"multi_level", multi_level_count}},
      1, kLevels, static_cast<int>(live_ids.size()),
      Json{{"fill_count", fill_tracker.fill_count},
           {"filled_quantity_units", fill_tracker.filled_quantity_units}},
      resource.allocations() - allocations_before_measured,
      "build/release/cpp/exchange/app/aegis_exchange_bench --workload policy_mix --seed " +
          std::to_string(seed));
}

}  // namespace

int main(int argc, char** argv) try {
  std::string workload;
  std::uint64_t seed = 1729;
  int k = 8;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (arg == "--workload" && i + 1 < argc) {
      workload = argv[++i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    } else if (arg == "--seed" && i + 1 < argc) {
      seed = std::stoull(argv[++i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    } else if (arg == "--k" && i + 1 < argc) {
      k = std::stoi(argv[++i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }

  Json report;
  if (workload == "lookup_cancel") {
    report = run_lookup_cancel(seed);
  } else if (workload == "single_fill_aggressor") {
    report = run_single_fill_aggressor(seed);
  } else if (workload == "multi_fill_multi_level_aggressor") {
    report = run_multi_fill_multi_level_aggressor(seed, k);
  } else if (workload == "policy_mix") {
    report = run_policy_mix(seed);
  } else {
    std::cerr
        << "usage: aegis_exchange_bench --workload "
           "{lookup_cancel,single_fill_aggressor,multi_fill_multi_level_aggressor,policy_mix} "
           "[--seed N] [--k N]\n";
    return 2;
  }

  std::cout << report.dump(2) << "\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "aegis_exchange_bench: " << error.what() << "\n";
  return 2;
}
