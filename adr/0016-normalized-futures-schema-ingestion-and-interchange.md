# ADR-0016: Normalized futures schema, ingestion and columnar interchange

- Status: Accepted
- Date: 2026-08-10
- Requirement IDs: AEGIS-026, AEGIS-014, AEGIS-025, AEGIS-230
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

## Slice 5 addendum: data quality and columnar interchange (AEGIS-025, AEGIS-014 completion, AEGIS-230)

### Context

Slice 4 drew a boundary and deferred one side of it: ingestion judges
*shape* (can this be parsed onto the tick grid, is the timestamp explicit),
quality judges *validity* (is this well-formed record actually believable).
AEGIS-014's acceptance -- "identifies missing, stale, and contradictory
values" -- and AEGIS-025's -- "duplicates, gaps, invalid prices, impossible
timestamps, and contract metadata conflicts" -- both describe the second
kind of judgement, over data ingestion already accepted. AEGIS-230
(columnar interchange) was registered as an M0 deferred-verification
obligation, blocked on exactly the schema this slice's predecessor built.

### Decision

**Nine detector categories, matching the frozen wording, no invented tenth.**
`python/futures/quality.py`'s `run_quality_checks` implements exactly the
categories AEGIS-014 and AEGIS-025's acceptance text names:
`duplicate_observation` (a `(contract_symbol, event_time_ns)` repeat --
deliberately a *different* identity than ingestion's
`(contract_symbol, source_sequence)` duplicate check, because two records
can share a source-sequence-distinct identity while still claiming to
observe the same instant, which is exactly the kind of contradiction a
shape-only check cannot see), `gap`, `invalid_price` (non-positive), a
`contradictory_ohlc` category split out from `invalid_price` because the
M2 prompt names it as its own bullet, `impossible_timestamp` (outside the
contract's own `first_trade_date`..`expiry` window, `python/futures/chain.py`
providing the lookup), `contract_metadata_conflict`, `missing_volume`,
`missing_open_interest`, and `stale_observation` (an identical, zero-volume
OHLC shape repeated at or past a configurable threshold).

The prompt names a *tenth*, conditional category -- "contradictory
volume/OI values **where frozen criterion requires**" -- and it is
deliberately **not implemented**. Neither AEGIS-014's nor AEGIS-025's frozen
acceptance text names a specific cross-field volume/open-interest rule, and
inventing one here would be exactly the kind of unrequested economic
assertion this same milestone's roll-policy guidance warns against
elsewhere ("do not assert economically false... properties merely because
they are easy"). The one volume/OI constraint that does exist --
non-negativity -- already lives at ingestion (shape), not here (quality);
duplicating it as a "quality" finding would mean the detector can never
fire on any record that reached this module, which is dead code presented
as a control.

**Detection runs on data that skipped, or survived, ingestion -- not
necessarily normalized-and-clean data.** The seeded corruption fixture
(`tools/seeded_quality_corruptions.py`, shared by the evidence generator and
the test suite so there is exactly one claim, not two that could drift)
builds records directly rather than through `ingest()`, because the
categories under test -- a non-positive price, an internally contradictory
OHLC relationship, a timestamp outside the contract's window -- are
precisely the well-formed-but-wrong values ingestion's shape checks do not
reject.

**Gap detection is opt-in, with an explicit expected interval.** Guessing a
series' cadence from the data itself (e.g. the median delta) would make the
detector's sensitivity depend on how much bad data was already present --
circular. The caller states the expected spacing explicitly (as every roll
policy in slice 6 states its own parameters explicitly); without it, no
`GAP` issues are produced at all, which is itself observable in the report
rather than a silent zero.

**Severity is two-valued: `ERROR` for a genuinely contradictory or
unresolvable record, `WARNING` for an absence or a statistical anomaly** --
missing volume/OI, a gap, a stale run. Nothing in AEGIS-014/025's frozen
text asks for more granularity, and a third level would be a judgement call
with no acceptance criterion to anchor it.

**Columnar interchange: one schema throughout, no second Arrow-specific
shape.** `python/futures/columnar.py`'s `to_arrow_table` writes exactly
`futures.schema.NORMALIZED_COLUMNS`, in that fixed order, with the schema
name and version embedded in the Arrow table's own metadata --
`pyarrow.parquet.write_table` carries Arrow schema metadata into the Parquet
file's footer automatically, so nothing bespoke is needed to make the
version travel with the file. `read_parquet` refuses a file whose declared
name/version this build does not recognize, matching every other AEGIS
schema's "reject, never reinterpret" rule. `query_duckdb` runs SQL directly
against the same Parquet file through a DuckDB view -- no separate DuckDB-
native copy, so there is exactly one persisted representation of the data
to trust.

**AEGIS-230's obligation is discharged, not the broader requirement
over-promoted.** The M0-registered obligation named a concrete residual --
"Parquet, Arrow and DuckDB round trips are not implemented... building them
now would require inventing the futures schema AEGIS-026 owns." That schema
now exists (slice 4) and the round trip is built and proven (this slice), so
the obligation is cleared via `tools/update_status.py --clear-obligation`.
AEGIS-230's catalogue status is otherwise unaffected by this milestone --
paying the M2 obligation is not the same act as promoting the requirement
to `verified`, which remains M2 closure's decision.

### Alternatives considered

- **A single "bad price" category covering both non-positive values and
  internal OHLC contradictions** -- rejected: the M2 prompt lists
  "invalid prices" and "contradictory OHLC relationships" as separate
  bullets, and a record can be wrong in exactly one of the two ways (a
  positive but internally-inconsistent bar; a non-positive but internally
  self-consistent one), which is worth reporting distinctly.
- **Inferring an expected gap interval from the data** -- rejected as
  circular; see Decision.
- **A negative-volume/negative-open-interest "contradictory" quality
  category** -- rejected as unreachable dead code once ingestion's own
  shape check already excludes it; see Decision.
- **A DuckDB-native table as the columnar store, with Parquet as an
  export format** -- rejected: it would create two persisted
  representations of the same data with no single source of truth, and
  AEGIS-230's acceptance is a round trip over one schema, not two storage
  engines.
- **Storing schema version only as a Parquet key-value pair added by hand**
  (bypassing Arrow table metadata) -- rejected: `pyarrow` already carries
  Arrow schema metadata into Parquet automatically, so a hand-rolled path
  would be a second, redundant mechanism for the same fact.

### Consequences

- Every later M2 slice (roll policies, continuous series) that wants a
  quality-checked view of the data calls `run_quality_checks` explicitly;
  none of them run it implicitly, so a caller that skips quality checking
  is visible in its own code, not hidden inside `ingest()`.
- The nine-category list is now the frozen surface other slices build
  audit/reporting tooling against (e.g. a later roll-audit report, AEGIS-023,
  M2 slice 8) -- adding a tenth category later is an ADR-worthy decision,
  not a quiet addition.
- AEGIS-229 (C++/Python bindings), the other M2-due deferred obligation, is
  untouched by this slice -- it belongs to slice 13.

### Verification

- `tests/unit/test_futures_quality.py` -- one or more dedicated tests per
  detector category (including both `CONTRADICTORY_OHLC` violation shapes,
  the `STALE_OBSERVATION` threshold boundary, gap enabled/disabled), a
  determinism-of-ordering check, and the seeded-corruption suite proving all
  nine categories are actually caught by the production function.
- `tests/property/test_quality_detectors.py` -- no false positives on
  arbitrary well-formed unique records; deterministic and order-independent
  results; every reported identifier names a record that was actually
  supplied.
- `tests/integration/test_columnar_roundtrip.py` -- the full round trip
  over the three committed families: row count, byte-identical values,
  exact integer ticks, `record_index`, `event_time_ns`, contract identity,
  nullability, deterministic column order, schema-version metadata,
  rejection of an unknown/missing schema version, and a DuckDB query
  agreeing with Arrow's own row count.

## Owner approval

Authorized as part of M2 slice 4 (schema and ingestion, AEGIS-026 and
AEGIS-014's ingestion half) and M2 slice 5 (this addendum: quality and
columnar interchange, AEGIS-025, AEGIS-014's completion, AEGIS-230), both
under the owner-approved M2 plan of record (`experiments/plans/M2.md`,
rev. 4) and the owner's slice 3-7 continuous-execution prompt, 2026-08-10.
