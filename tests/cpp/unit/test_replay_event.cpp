#include <algorithm>
#include <compare>
#include <cstdint>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "cpp/replay/replay_event.hpp"

// ---------------------------------------------------------------------------
// M2 slice 1 — the canonical replay order.
//
// Deterministic replay rests entirely on the input having exactly ONE legal
// order. A comparator that merely "works" is not enough: if it is only a
// partial order then two correct sorts can disagree, and the disagreement
// surfaces much later as a replay digest mismatch that reads like an engine
// bug. So these tests assert the order-theoretic properties directly, not just
// a handful of expected sequences.
// ---------------------------------------------------------------------------

namespace {

using aegis::common::EventTime;
using aegis::replay::canonical_compare;
using aegis::replay::canonical_less;
using aegis::replay::RecordIndex;
using aegis::replay::ReplayEvent;
using aegis::replay::SourceSequence;

ReplayEvent make(std::int64_t time, std::uint64_t sequence, std::string symbol,
                 std::uint64_t index) {
  return ReplayEvent{.event_time = EventTime{time},
                     .source_sequence = SourceSequence{sequence},
                     .contract_symbol = std::move(symbol),
                     .record_index = RecordIndex{index}};
}

std::vector<ReplayEvent> sample_events() {
  return {
      make(1000, 7, "ESZ26", 3), make(1000, 7, "ESZ26", 1), make(1000, 5, "ESU26", 8),
      make(999, 99, "NGF27", 0), make(1000, 7, "ESU26", 2), make(1001, 1, "ESZ26", 4),
  };
}

// Records agreeing on the first three components — the case a timestamp-only
// sort cannot resolve.
std::vector<ReplayEvent> four_way_tie() {
  return {make(1000, 7, "ESZ26", 3), make(1000, 7, "ESZ26", 1), make(1000, 7, "ESZ26", 2),
          make(1000, 7, "ESZ26", 0)};
}

// Strong identifier types cannot be silently swapped. SourceSequence is an
// observation about the input; RecordIndex is assigned by ingestion. Conflating
// them would corrupt the tie-break, so a runtime test could not express this.
static_assert(!std::is_convertible_v<SourceSequence, RecordIndex>);
static_assert(!std::is_convertible_v<RecordIndex, SourceSequence>);
static_assert(!std::is_convertible_v<std::uint64_t, RecordIndex>);
static_assert(!std::is_convertible_v<std::uint64_t, SourceSequence>);

TEST(ReplayEventOrder, EventTimeDominates) {
  EXPECT_TRUE(canonical_less(make(999, 9999, "ZZZZZ", 9999), make(1000, 5, "ESZ26", 10)));
}

TEST(ReplayEventOrder, SourceSequenceBreaksAnEventTimeTie) {
  EXPECT_TRUE(canonical_less(make(1000, 4, "ZZZZZ", 9999), make(1000, 5, "ESZ26", 10)));
}

TEST(ReplayEventOrder, ContractSymbolBreaksASequenceTie) {
  EXPECT_TRUE(canonical_less(make(1000, 5, "ESU26", 9999), make(1000, 5, "ESZ26", 10)));
}

TEST(ReplayEventOrder, RecordIndexBreaksASymbolTie) {
  EXPECT_TRUE(canonical_less(make(1000, 5, "ESZ26", 9), make(1000, 5, "ESZ26", 10)));
}

TEST(ReplayEventOrder, RecordIndexIsWhatMakesTheOrderTotal) {
  // Without it these four are indistinguishable and any permutation is a valid
  // sort — precisely the ambiguity that breaks byte-identical replay.
  const auto events = four_way_tie();
  for (const auto& lhs : events) {
    for (const auto& rhs : events) {
      const bool same_record = lhs.record_index == rhs.record_index;
      EXPECT_EQ(canonical_compare(lhs, rhs) == std::strong_ordering::equal, same_record);
    }
  }
}

TEST(ReplayEventOrder, IsIrreflexive) {
  for (const auto& event : sample_events()) {
    EXPECT_FALSE(canonical_less(event, event));
    EXPECT_TRUE(canonical_compare(event, event) == std::strong_ordering::equal);
  }
}

TEST(ReplayEventOrder, IsTrichotomous) {
  const auto events = sample_events();
  for (const auto& lhs : events) {
    for (const auto& rhs : events) {
      const int less = canonical_less(lhs, rhs) ? 1 : 0;
      // The swap is the point: asymmetry means exactly one of less/greater/
      // equal holds for every pair.
      // NOLINTNEXTLINE(readability-suspicious-call-argument)
      const int greater = canonical_less(rhs, lhs) ? 1 : 0;
      const int equal = (canonical_compare(lhs, rhs) == std::strong_ordering::equal) ? 1 : 0;
      EXPECT_EQ(less + greater + equal, 1);
    }
  }
}

TEST(ReplayEventOrder, IsTransitive) {
  const auto events = sample_events();
  for (const auto& a : events) {
    for (const auto& b : events) {
      for (const auto& c : events) {
        if (canonical_less(a, b) && canonical_less(b, c)) {
          EXPECT_TRUE(canonical_less(a, c));
        }
      }
    }
  }
}

TEST(ReplayEventOrder, SortIsIndependentOfInputOrderAndOfStability) {
  // The canonical order is a property of the records, not of how they reached
  // the sorter. A stable and an unstable sort must agree, or replay would
  // depend on a standard-library implementation detail.
  const std::vector<ReplayEvent> canonical{
      make(999, 99, "NGF27", 0), make(1000, 5, "ESU26", 8), make(1000, 7, "ESU26", 2),
      make(1000, 7, "ESZ26", 1), make(1000, 7, "ESZ26", 3), make(1001, 1, "ESZ26", 4),
  };

  auto shuffled = canonical;
  // A constant seed is deliberate. This test asserts that sorting is invariant
  // under input permutation; a per-run seed would make a failure unreproducible,
  // which is the opposite of what a determinism suite needs.
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 generator(20260810);
  for (int round = 0; round < 32; ++round) {
    std::ranges::shuffle(shuffled, generator);

    auto unstable = shuffled;
    std::ranges::sort(unstable, canonical_less);
    auto stable = shuffled;
    std::ranges::stable_sort(stable, canonical_less);

    for (std::size_t i = 0; i < canonical.size(); ++i) {
      EXPECT_TRUE(canonical_compare(unstable[i], canonical[i]) == std::strong_ordering::equal);
      EXPECT_TRUE(canonical_compare(stable[i], canonical[i]) == std::strong_ordering::equal);
    }
  }
}

TEST(ReplayEventOrder, SymbolComparisonIsByteWiseNotLocaleAware) {
  // A locale-sensitive collation would make the canonical order depend on the
  // machine's environment. Uppercase precedes lowercase in ASCII; several
  // locales disagree.
  EXPECT_TRUE(canonical_less(make(1, 1, "ESZ26", 0), make(1, 1, "esz26", 0)));
  EXPECT_TRUE(canonical_less(make(1, 1, "ES", 0), make(1, 1, "ESZ26", 0)));
}

}  // namespace
