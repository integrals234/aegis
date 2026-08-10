# ADR-0005: Toolchain, environments and the C++/Python boundary

- Status: Accepted
- Date: 2026-08-06
- Requirement IDs: AEGIS-009, AEGIS-227, AEGIS-228, AEGIS-229
- Milestone: M0

## Context

`docs/ARCHITECTURE.md` assigns deterministic and latency-sensitive cores to C++
and research, orchestration and reporting to Python. That split only holds if
the boundary between them is narrow and if both environments are reproducible;
otherwise the convenient thing to do from Python grows until Python is driving
the engine.

## Decision

**C++.** C++20, no compiler extensions, Ninja, and three presets: `debug`,
`release` and `asan-ubsan`. Warnings are errors and include `-Wconversion`,
`-Wsign-conversion`, `-Wold-style-cast` and `-Wshadow`. The sanitizer preset uses
`-fno-sanitize-recover=all`, so a memory or UB error fails the build rather than
reappearing later as replay nondeterminism. `.clang-tidy` enables the families
that catch what this project cannot tolerate, and every disabled check carries
its reason in the file.

C++23 was not adopted. `std::expected` would have been convenient for the
envelope decoder, and a hand-rolled result type was written instead: changing the
language standard is a decision with its own consequences for the toolchains
that must build AEGIS, not a side effect of writing one file.

**Third-party C++.** GoogleTest and nlohmann/json are fetched by URL with a
pinned SHA-256. A tag can be moved; a hash cannot.

**Python.** CPython 3.12 is the declared floor and CI matrix baseline; 3.14 is
the development host and is also tested. `tools/probe_dependencies.py` proves
from PyPI metadata that every pinned distribution has a wheel for each supported
interpreter before either is adopted, and the verdict is committed as evidence.
Dependencies are hash-pinned in `requirements/requirements.lock`; installs use
`--require-hashes`.

`clang-format` and `clang-tidy` are installed from the Python lockfile rather
than the distribution, so the version gating CI is the version a developer runs
and a static-analysis finding cannot depend on whose machine ran it.

**Bindings.** pybind11 is the documented equivalent AEGIS-229 permits, confined
to `cpp/bindings` — the architecture checker rejects pybind11 or `Python.h`
anywhere else. The binding policy:

> No binding may directly mutate engine internals or execute on the
> latency-critical path. Stateful operations required for replay, experiments and
> paper trading must use explicit versioned command interfaces routed through the
> same risk and OMS boundaries as any other participant action.

A binding that handed Python a mutable pointer into a book would make the
strategy → risk → OMS path optional from the Python side, which AEGIS-120
forbids. The M0 surface is five pure functions — `version`, `build_info`,
`envelope_schema_version`, `encode_envelope`, `decode_envelope` — and a test
asserts that exact set, so an addition that breaks the policy has to break a test
first.

The bindings layer may depend only on `cpp-common` and `cpp-events`, so there is
no exchange or participant code for a binding to reach.

**Ratified deviation (owner, 2026-08-06).** The approved M0 plan specified
`version()` and `build_info()` only. The implementation also exposes
`envelope_schema_version()`, `encode_envelope()` and `decode_envelope()`. The
owner ratified the addition after the independent M0 audit, on these grounds: the
three additions are pure functions over `cpp/events`, they touch no engine state
and no latency-critical path, they stay inside the bindings layer's declared
dependencies, and they turn AEGIS-229's round-trip acceptance into a live
comparison between the C++ and Python encoders rather than two assertions against
a stored golden file — a strictly stronger cross-language guarantee. The policy
above is unchanged; the surface test is what keeps it enforced.

## Alternatives considered

**C++23 for `std::expected`, `std::print` and ranges improvements.** Deferred:
it narrows the set of toolchains that can build AEGIS for a convenience gain,
and the one place it was wanted is three lines of hand-rolled type.

**pybind11 via FetchContent.** Rejected: a second, separately versioned copy of
a dependency the Python environment already pins.

**Cython or ctypes.** Rejected: ctypes loses type safety at exactly the boundary
that needs it, and Cython adds a second build system for the same job.

**A single interpreter version.** Rejected: a supported range that is never
tested is an intention, and the failure appears when somebody upgrades.

**Exposing config and metrics through the bindings at M0.** Deferred to M2,
where the roadmap places the bindings work for data and replay. Binding an
engine that does not exist would be building M1 early.

## Consequences

- `-Wconversion` and `-Wsign-conversion` require explicit casts in size and
  index arithmetic. This is deliberate: silent narrowing in price and quantity
  arithmetic is a correctness bug in a trading system.
- Three presets means three builds in CI.
- `CMAKE_POSITION_INDEPENDENT_CODE` is on so the layer libraries link into the
  bindings module; building them twice would let the tested code and the bound
  code diverge.
- A dependency change is a four-step procedure (probe, lock, install, verify)
  documented in `docs/ENVIRONMENT.md`.

## Verification

- `scripts/check_environment.sh` checks versions, not just presence, and
  dry-run-installs the lockfile to detect drift.
- `scripts/check_cpp_style.sh` runs clang-format and clang-tidy over tracked and
  staged sources.
- `tests/integration/test_bindings_roundtrip.py` asserts the loaded module is
  the compiled extension and that the exported surface is exactly the five
  permitted functions.
- Evidence: `experiments/evidence/AEGIS-228/`, `experiments/evidence/AEGIS-009/`.

## Slice 13 addendum: `sort_canonical` and the `cpp-replay` dependency edge (AEGIS-229, AEGIS-059)

### Context

M2 slice 13 is the first time the bindings layer needs to reach past
`cpp-common`/`cpp-events`. AEGIS-229's acceptance calls for round-trip
integration tests over "selected engine APIs"; slice 9's replay core
(`cpp/replay/replay_event.hpp`) is the first M2 API worth exercising that
way, since it is a pure function of plain data (sort records into the
canonical order) with no engine state to protect.

### Decision

**`configs/architecture_rules.yaml` gains one edge: `cpp-bindings` may now
depend on `cpp-replay`, and only `cpp-replay`.** No `cpp-exchange-*` or
`cpp-participant-*` layer is added — the binding does not widen past what
M2 actually needs bound, and the existing rule that `cpp-replay` itself
has no edge to any exchange layer is untouched.

**`sort_canonical(list[dict]) -> list[dict]`** constructs real
`aegis::replay::ReplayEvent` objects from dicts shaped like slice 1's four
canonical fields (`event_time_ns`, `source_sequence`, `contract_symbol`,
`record_index`), sorts them with the real `canonical_less`, and returns
sorted dicts. It mutates nothing — no `ReplayEngine`, `VirtualClock`,
pacing or fault-injection state is bound, matching the policy above
exactly: this is a pure function over plain data, not a stateful replay
operation routed around risk/OMS.

**The M0 surface of five pure functions (Decision, above) grows to six.**
`tests/integration/test_bindings_roundtrip.py::test_bindings_expose_no_mutable_engine_state`
now asserts the six-function set including `sort_canonical`; the addition
had to break that test first, exactly as the original five did.

**The Python peer (`python/futures/replay.py::sort_canonical`) is a
separate, native implementation, not a wrapper around the binding.** A
feed usable without a C++ toolchain is worth more than saving one
`sorted()` call; a dedicated symmetry test
(`test_cpp_sort_canonical_matches_the_python_peer`) proves the two agree
on the same input rather than assuming it because one calls the other.

### Verification

- `tests/integration/test_bindings_roundtrip.py` — the six-function
  surface assertion, `sort_canonical` agreement with the Python peer
  across empty/single/tied/pre-epoch cases, and a real-sort-not-identity
  check.
- `tools/check_architecture.py` — passes with the new `cpp-bindings` →
  `cpp-replay` edge and no other new edge.

## Owner approval

Recorded in the approved M0 plan (`experiments/plans/M0.md`, P3 and Part 6). The
widened binding surface was ratified separately by the owner on 2026-08-06,
following the independent M0 audit; see the ratified deviation above. The
slice 13 addendum above is authorized under the owner's M2 slice 8-13
build-first continuous-execution prompt and the approved M2 plan of record
(`experiments/plans/M2.md`, rev. 4), 2026-08-10.
