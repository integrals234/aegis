# ADR-0016: Normalized futures schema, ingestion and columnar interchange

- Status: Accepted
- Date: 2026-08-10
- Requirement IDs: AEGIS-026, AEGIS-014
- Milestone: M2

## Context

Every later M2 slice -- quality (AEGIS-025, slice 5), roll policies
(AEGIS-015..018, slice 6), continuous series (AEGIS-019..022, slice 7) --
needs one normalized shape to operate over, from any of the three
(eventually more) synthetic product families. Two failure modes to design
against: a second, parallel schema definition drifting from the first (the
same defect `python/data/schema_registry.py` exists to prevent, at M0, for
exactly this eventual moment), and a canonical price path that loses
exactness the way a binary float loses it on `0.1` -- which M2 slice 2's
`Decimal` discipline for `tick_size`/`multiplier` was chosen specifically to
keep lossless into.

This slice (M2 slice 4) covers the schema and ingestion half: AEGIS-026 (one
normalized interface for >= 3 product families) and AEGIS-014's ingestion
portion (volume/open-interest fields ingested and validated for *shape*).
AEGIS-014's data-quality-report half and AEGIS-025 (bad-data detection) are
M2 slice 5, and this ADR is extended below when that slice lands, together
with AEGIS-230's columnar interchange decision.

## Decision

**One schema, `futures_bar.v1`, registered through the existing
`SchemaRegistry` (AEGIS-230's M0 half).** `configs/schemas/futures_bar.v1.json`
carries `x-aegis-schema-name`/`x-aegis-schema-version` (what
`SchemaRegistry.register_file` reads) and its own `schema_version` property
(what every record must declare). `python/futures/schema.py` is the only
module that builds the registry for this schema and the only place
`NORMALIZED_COLUMNS` -- the fixed, explicit column order every writer of this
shape (CSV fixtures, and slice 5's columnar output) shares -- is defined.
This reuses the registry rather than re-implementing version-validation
logic a second time, per the M2 plan of record.

**Integer tick prices, converted by exact division.** OHLC and settlement are
`open_ticks`/`high_ticks`/`low_ticks`/`close_ticks`/`settlement_price_ticks` --
plain integers, never floats. `python/futures/ingest.py` converts each
decimal-string input price by dividing by the resolved `Product.tick_size`
(`python/futures/instruments.py`, `Decimal` since M2 slice 2) and requiring
the quotient have no fractional part; a price that is not an exact multiple
of the product's tick grid is rejected as malformed, never rounded. This is
the "eventual projection onto M1's integer tick/lot grid" ADR-0015 named as
the reason `tick_size` stays `Decimal` all the way from the product catalog.

**`event_time_ns` must already be an explicit UTC integer.** Ingestion does
not parse or infer a timestamp from any string format -- accepting one would
mean guessing a timezone or a naive-local convention, which is exactly the
kind of ambiguity `ContractId`'s four-digit year exists to refuse elsewhere
in this same module family. A non-integer value (a float, an ISO string) is
rejected outright as the "naive/ambiguous timestamp" case the M2 plan of
record names.

**`contract_symbol` accepts only the canonical form.** Raw input rows carry
`VENUE:ROOT:YYYYM`, parsed by `ContractId.parse` (M2 slice 1). Vendor-specific
alternate symbol spellings are explicitly out of scope: `identifiers.py`'s own
docstring already draws this boundary ("vendor-specific symbol formats are a
normalization concern... and letting them in here would mean two spellings
of one contract could both parse"), and this slice does not build a second,
competing symbol-mapping layer to cross it. If a future slice needs vendor
formats, it is a new, explicit normalization step upstream of this one, not a
loosening of `ContractId.parse`.

**Malformed policy: STRICT (default) or REPORT, chosen by the caller.**
STRICT raises `IngestError` on the very first rejection, in deterministic
`(source_file, physical_position)` order -- independent of what order the
caller listed input paths in. REPORT collects every rejection
(`Rejection(location, kind, field, reason)`) instead and excludes those
records from the surviving set. Neither policy silently drops a record: a
rejection is always either raised or recorded, never absorbed.

**Duplicates: `(contract_symbol, source_sequence)`, always observable.** A
repeat of that pair is rejected under the same STRICT/REPORT policy as a
malformed record -- the first occurrence (in deterministic order) survives,
every later one is recorded as a `"duplicate"` rejection. Nothing is
silently collapsed.

**Out-of-order input stays observable, not silently resorted.** Ingestion
does not re-sort records into canonical replay order -- that is a
replay-time concern for a later slice. It does record every position where
`(event_time_ns, source_sequence)` decreases relative to file encounter
order, so a source feed's own ordering defects remain visible in the
ingestion result rather than disappearing into a normalized-and-therefore-
apparently-fine artifact.

**`record_index`, exactly per the M2 plan of record's section 7.** Input
paths are made repository-relative POSIX (falling back to an absolute path
for anything outside the repository, e.g. a test fixture), deduplicated and
sorted lexicographically; each row keeps its 1-based physical line number;
malformed/duplicate policy runs in that `(source_file, physical_position)`
order; `record_index` is assigned to the *surviving* records, in the same
order, contiguously from zero. It is persisted as an explicit field and is
never recomputed by any downstream reader -- replay (a later slice) reads it.

## Alternatives considered

- **A hand-rolled validator instead of `SchemaRegistry`** -- rejected: M0
  built exactly this registry for exactly this moment (AEGIS-230's
  versioning half); a second implementation would drift from it the first
  time one changed and the other did not.
- **Rounding an off-tick price instead of rejecting it** -- rejected: silent
  rounding is a silent repair of the input, which CLAUDE.md forbids, and it
  would corrupt every downstream notional/return calculation that assumes
  the tick grid is exact.
- **Parsing common timestamp string formats (ISO-8601, epoch seconds)** --
  rejected for this slice: every format choice smuggles in a timezone or
  epoch-unit assumption, and the frozen acceptance does not require it. A
  producer that wants a different timestamp convention converts to UTC
  nanoseconds upstream of this module.
- **Accepting vendor-specific contract symbol spellings** -- rejected,
  matching `identifiers.py`'s own documented boundary; see Decision above.
- **Treating a duplicate `(contract_symbol, source_sequence)` as "last one
  wins"** -- rejected: it would silently discard the fact that the source
  data contained a contradiction, which is the opposite of AEGIS-014's
  "identifies... contradictory values" acceptance wording.
- **Re-sorting into canonical replay order at ingestion time** -- rejected:
  it would erase the fact that a source feed arrived out of order, and
  canonicalizing order is a replay-time (later-slice) responsibility per the
  M2 plan of record, not an ingestion one.

## Consequences

- `python-futures` now has its first genuine edge to `python-data`
  (`configs/architecture_rules.yaml` already permitted it; nothing before
  this slice used it) -- `python/futures/schema.py` imports
  `data.schema_registry`.
- Slice 5's quality detectors and columnar writer both consume
  `NORMALIZED_COLUMNS`/`futures_bar.v1` as already-fixed facts; neither
  redefines the shape.
- A negative or economically implausible but well-formed price (say, a
  negative settlement) is *not* rejected here -- that is a quality judgement,
  AEGIS-025's job (slice 5), not a shape judgement. Conflating the two would
  make "could not be parsed" indistinguishable from "parsed but suspicious",
  which the M2 plan of record's slice 5 section explicitly warns against.

## Verification

- `tests/unit/test_futures_schema.py` -- registry wiring, valid/invalid
  records, nullable fields, unknown schema version, additional-property
  rejection, column/required-field agreement.
- `tests/unit/test_futures_ingest.py` -- CSV and JSONL normalization,
  malformed-field rejection for every field, malformed-JSON-line rejection,
  unsupported-extension rejection, duplicate detection under both policies,
  out-of-order detection, `record_index` contiguity/no-duplicates/
  sorted-file-order/shuffled-argument-order/repeated-ingestion determinism,
  a cross-process `PYTHONHASHSEED` proof, and that STRICT reports the exact
  source location of the first rejection.
- `tests/integration/test_three_family_load.py` -- the three *committed*
  bar fixtures (not synthetic unit-test data) loaded through the real
  `load_catalog`/`ingest` production path, schema-validated, contiguous
  `record_index`, no floats on the canonical path.
- `tests/property/test_normalization_idempotence.py` -- for arbitrary
  well-formed rows: repeated ingestion is byte-identical, every normalized
  record validates, tick conversion is exact and `record_index` is
  contiguous.

## Owner approval

Authorized as part of M2 slice 4 under the owner-approved M2 plan of record
(`experiments/plans/M2.md`, rev. 4) and the owner's slice 3-7
continuous-execution prompt, 2026-08-10.
