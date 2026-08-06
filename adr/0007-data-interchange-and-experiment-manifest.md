# ADR-0007: Data interchange, sample data and the experiment manifest

- Status: Accepted
- Date: 2026-08-06
- Requirement IDs: AEGIS-230, AEGIS-236, AEGIS-009
- Milestone: M0

## Context

AEGIS-230 asks for Parquet/Arrow and DuckDB "where suitable, with versioned
schemas". AEGIS-236 permits committing only small redistributable samples and
requires large or licensed data to stay external. AEGIS-097 and AEGIS-209..216
require every research run to record what it would take to reproduce it.

M0 has no futures data, no research runs and no columnar datasets. The temptation
is to build the columnar layer now so the requirement can be ticked; the cost is
inventing a futures schema, which is AEGIS-026 and belongs to M2.

## Decision

**Schema versioning now, columnar formats at M2.** Every record carries
`schema_version`; a reader refuses a version it does not know rather than
interpreting it under today's field meanings. Versions coexist so a migration can
be gradual, and compatibility is recorded per version rather than inferred —
the inference is exactly the judgement call made optimistically under deadline.
JSON Lines and CSV codecs ship now, both with fixed ordering so a dataset is
diffable and hashable. Parquet/Arrow/DuckDB arrive with M2's data.

CSV integer columns are declared by the caller, not guessed: guessing turns an
instrument code like `007` into `7`.

**Sample data.** A committed sample must be small, of an allowed type, and carry
provenance including an explicit `redistributable: true`. The M0 sample is
*generated* by `tools/make_sample_data.py` rather than sliced from a feed — a
small excerpt of licensed market data is still licensed, so the reliably
redistributable artifact is one containing no market data at all. The generator
is seeded, so re-running it reproduces the committed file byte for byte.

**External datasets** are referenced in `configs/external_datasets.yaml` by
immutable version identifier with a checksum and access instructions. A version
of `latest`, `current` or a branch name is rejected: the data changes, the
research does not, and the mismatch surfaces as a result nobody can reproduce.
M0 registers none, because no research has run and pre-registering a feed would
record an intention rather than a fact.

**Experiment manifest.** `configs/schemas/experiment_manifest.v1.json` requires
experiment id, code commit, resolved-config digest, seed, creation time and a
rerun command, and optionally the data version, date range, contracts, roll
method, costs, environment and artifacts. The **resolved** configuration is
hashed, not the file: environment and CLI overrides change what actually ran.
`git_commit()` appends `-dirty` for a modified tree, because naming a clean
commit for a dirty tree is worse than naming none — it looks reproducible.

Optional fields are omitted rather than emptied: an empty `roll_method` reads as
"no roll method", an absent one as "this run had none", and research comparing
roll conventions depends on telling them apart.

An invalid manifest never reaches disk. Writing first and validating later leaves
an unusable artifact somebody will later find and trust.

The experiment **registry** — storage, listing, artifact lookup, rerun
generation — is M9 (AEGIS-215, AEGIS-216).

## Alternatives considered

**Building the Parquet round trip now with a placeholder schema.** Rejected:
the placeholder would be a futures schema invented outside the requirement that
owns it, and it would have to be thrown away or lived with.

**Committing a small slice of real market data.** Rejected: a slice of licensed
data is licensed, and AEGIS-236 permits only redistributable samples.

**Hashing the configuration file rather than the resolved configuration.**
Rejected: it silently omits every environment and CLI override, which is where
the interesting differences between runs live.

**Making the manifest schema permissive to avoid friction.** Rejected: the
friction is the feature. A manifest missing its commit is discovered at write
time, not months later when the run needs reproducing.

## Consequences

- `*.parquet` and `*.arrow` remain gitignored outside `data_samples/`; M2
  revisits that when a binary fixture is actually needed.
- Every dataset gains a `schema_version` field, including in CSV.
- A research run cannot be recorded without a commit, a config digest, a seed
  and a rerun command.

## Verification

- `tests/unit/test_schema_registry.py`, including the committed sample
  round-tripping through the CSV codec.
- `tests/unit/test_check_sample_data.py`, covering both halves of AEGIS-236.
- `tests/research/test_experiment_manifest.py`, including the dirty-tree marker
  and the resolved-versus-file digest distinction.

## Owner approval

Recorded in the approved M0 plan (`experiments/plans/M0.md`, Part 6, ADR-0007).
