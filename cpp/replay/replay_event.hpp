#pragma once

#include <compare>
#include <cstdint>
#include <string>

#include "cpp/common/time.hpp"

/// The canonical replay record and the total order over it (M2 slice 1).
///
/// Layer `cpp-replay` may depend on `cpp-common` and `cpp-events` only
/// (`configs/architecture_rules.yaml`). It may **not** depend on any
/// `cpp-exchange-*` layer, in either direction, so replay never reaches into
/// matching or the book; the two are composed at the process boundary through
/// the existing `aegis_exchange_replay` CLI.
///
/// This file is the foundation the rest of M2's replay work is built on, and
/// deliberately nothing more: no stream reader, no virtual clock, no pacing, no
/// fault injection. Those are slices 9 through 12.
///
/// # Why ordering is a type-level concern
///
/// Deterministic replay means two runs over the same input emit byte-identical
/// output. That is only possible if the input has exactly one legal order, and
/// "sort by timestamp" does not give you one: real feeds contain many records
/// sharing a nanosecond. Whatever resolves those ties *is* the determinism
/// guarantee, so it lives here rather than being re-derived at each call site.
namespace aegis::replay {

/// Sequence number as it appeared in the source dataset.
///
/// Distinct from `events::CommandSequence` and `events::EventSequence`, which
/// the exchange assigns. This one is an observation about the input: it is
/// whatever the vendor or fixture wrote, may contain gaps, may restart, and may
/// repeat across contracts. It is an ordering key, never a trust anchor.
class SourceSequence {
 public:
  constexpr SourceSequence() = default;
  constexpr explicit SourceSequence(std::uint64_t value) : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const { return value_; }

  constexpr auto operator<=>(const SourceSequence&) const = default;
  constexpr bool operator==(const SourceSequence&) const = default;

 private:
  std::uint64_t value_{0};
};

/// Zero-based position of a normalized record within the canonical ingestion
/// result — and the deterministic final tie-breaker of the canonical order.
///
/// It is assigned once, by ingestion, in a single deterministic pass:
///
///   1. input paths are made repository-relative with POSIX separators,
///      deduplicated, and **sorted lexicographically** — so the caller's
///      argument order and the filesystem's enumeration order are irrelevant;
///   2. records carry their 1-based physical line (or 0-based array element);
///   3. normalization and the malformed/duplicate rejection policies run;
///   4. `record_index` is assigned over the **surviving** records in
///      `(source_file_id, line_number)` order, contiguously from 0.
///
/// It is then persisted in the canonical input artifact and covered by the
/// replay manifest's input digest. Replay **reads** it and never recomputes it.
///
/// Being a pure function of (input bytes, input path set, ingestion config), it
/// does not depend on memory layout, hash iteration order, `PYTHONHASHSEED`,
/// threading, scheduling, wall-clock time or build configuration — which is
/// what makes it identical across repeated ingestion, separate processes, debug
/// and release, snapshot/resume, and every pacing mode.
class RecordIndex {
 public:
  constexpr RecordIndex() = default;
  constexpr explicit RecordIndex(std::uint64_t value) : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const { return value_; }

  constexpr auto operator<=>(const RecordIndex&) const = default;
  constexpr bool operator==(const RecordIndex&) const = default;

 private:
  std::uint64_t value_{0};
};

/// One normalized observation, ready to be replayed.
///
/// `contract_symbol` is the canonical contract identifier produced by
/// `python/futures` (slice 2 formalises its structure). It participates in the
/// order so that two contracts sharing a timestamp and a source sequence — which
/// happens whenever a venue numbers its feeds per-product — still have exactly
/// one legal interleaving.
struct ReplayEvent {
  // No `{}` initialisers: every member's own default constructor already
  // value-initialises it, and clang-tidy's readability-redundant-member-init
  // is right that repeating it here is noise.
  common::EventTime event_time;
  SourceSequence source_sequence;
  std::string contract_symbol;
  RecordIndex record_index;
};

/// The canonical order: `(event_time, source_sequence, contract_symbol,
/// record_index)`.
///
/// This is a **strict total order**, and `record_index` is what makes it one.
/// Ties on the first three components are ordinary — a single nanosecond can
/// hold many records — so the key would otherwise be partial, and a partial key
/// admits more than one valid sort. `record_index` is unique by construction
/// across a canonical ingestion result, so no two distinct records can compare
/// equal here.
///
/// Equality of `ReplayEvent` under this comparison therefore means "the same
/// record", not "two records that happen to look alike".
[[nodiscard]] std::strong_ordering canonical_compare(const ReplayEvent& lhs,
                                                     const ReplayEvent& rhs);

/// Strict-weak-ordering predicate, for `std::sort` and friends.
///
/// Because the underlying order is total, sorting with this never depends on
/// the algorithm's stability — a stable and an unstable sort produce the same
/// sequence. Replay determinism must not rest on a library implementation
/// detail.
[[nodiscard]] bool canonical_less(const ReplayEvent& lhs, const ReplayEvent& rhs);

}  // namespace aegis::replay
