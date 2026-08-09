# Environment

Everything needed to build, test and reproduce AEGIS (AEGIS-009). If a step here
is missing, `scripts/check_environment.sh` should have caught it — that script is
the executable form of this document, and a discrepancy between the two is a bug
in one of them.

## Baseline

| Component | Requirement | Why this floor |
|---|---|---|
| OS | Linux (Ubuntu 24.04 reference) | The build uses `/proc` for environment capture and Linux CPU affinity for later benchmarks. |
| Python | ≥ 3.12 | The floor CI pins. `tools/probe_dependencies.py` proves every pinned dependency has a wheel for it. |
| CMake | ≥ 3.24 | Version 6 presets and the `FetchContent` usage in `tests/cpp/CMakeLists.txt`. |
| Ninja | ≥ 1.10 | The generator every preset selects. |
| Clang | ≥ 16 | C++20 support the substrate relies on, including `<concepts>` and designated initialisers. |
| Git | ≥ 2.30 | `--diff-filter` and `rev-parse --show-toplevel` behaviour the gates depend on. |

The development host at M0 is **WSL2 on Windows**, kernel
`6.6.114.1-microsoft-standard-WSL2`. `tools/capture_environment.py` detects this
and sets `virtualisation.bare_metal_claimable: false`. Per
[docs/BENCHMARK_POLICY.md](BENCHMARK_POLICY.md) rule 2, any figure measured here
must be labelled a WSL figure.

## Setting up

This block is the **canonical procedure** (AEGIS-009). It is the whole of what a
clean machine needs: no step is assumed, and nothing outside it is required.
`.github/workflows/ci.yml`'s `reproducibility` job runs exactly these commands
on a clean GitHub-hosted runner, so "the instructions work" is a checked fact
rather than a claim, and a step that exists only in CI is a bug in this
document.

```bash
git clone <repository> aegis && cd aegis

# 0. System toolchain. The Baseline table above states the floors; on the
#    Ubuntu 24.04 reference image this is the command that satisfies them.
#    python3-dev supplies the CPython headers pybind11 needs to build the
#    bindings module, and python3-venv the stdlib venv module step 1 uses.
#    clang-format and clang-tidy deliberately come from the lockfile in
#    step 1, not from apt.
sudo apt-get update
sudo apt-get install -y clang cmake ninja-build python3-dev python3-venv

# 1. Pinned Python environment. --require-hashes is not optional: without it,
#    a compromised or re-uploaded wheel installs silently.
python3 -m venv .venv
.venv/bin/pip install --upgrade pip
.venv/bin/pip install --require-hashes -r requirements/requirements.lock

# 2. Git hooks. CI runs the same checks independently, so a hook somebody forgot
#    to install is never the only gate.
bash scripts/install_git_hooks.sh

# 3. Verify.
export PATH="$PWD/.venv/bin:$PATH"
bash scripts/check_environment.sh
```

Step 3 is not decorative: `scripts/check_environment.sh` is the executable form
of this document, so if step 0 or step 1 is wrong or incomplete, it fails here
rather than three steps later inside a build.

### Where CI deliberately differs, and why that is not an extra step

The `reproducibility` job runs the procedure above exactly, using the runner's
own `python3`. Every other CI job additionally uses `actions/setup-python`,
which is a difference with a purpose rather than an undocumented step: those
jobs exist to test the **supported interpreter range**, and 3.14 is not present
on the `ubuntu-24.04` image. `tools/probe_dependencies.py` proves every pinned
dependency has a wheel for each version in that range.

| | Interpreter | What it establishes |
|---|---|---|
| `reproducibility` job | the machine's own `python3` | that this document is complete and correct on a clean machine |
| `python` matrix job | `actions/setup-python` 3.12 and 3.14 | that the pinned set works across the supported range |

Using `actions/setup-python` has one consequence worth knowing about locally
too: it exports `Python3_ROOT_DIR`, which outranks `PATH` in CMake's
`FindPython3`. That is why the build options below pin the interpreter
explicitly.

`clang-format` and `clang-tidy` are installed **from the Python lockfile**, not
from the distribution. That is deliberate: the version that gates CI is then
exactly the version a developer runs, and a static-analysis finding cannot depend
on whose machine ran it.

## Building and testing

Part of the canonical procedure: these are the commands CI runs, with the same
options.

```bash
export PATH="$PWD/.venv/bin:$PATH"

# AEGIS_CMAKE_OPTS is the same on every preset. Both options exist because of
# how Python is discovered; see "Why the two CMake options" below.
AEGIS_CMAKE_OPTS=(-DPython3_EXECUTABLE="$PWD/.venv/bin/python" -DAEGIS_REQUIRE_BINDINGS=ON)

cmake --preset debug "${AEGIS_CMAKE_OPTS[@]}"      && cmake --build --preset debug      && ctest --preset debug
cmake --preset release "${AEGIS_CMAKE_OPTS[@]}"    && cmake --build --preset release    && ctest --preset release
cmake --preset asan-ubsan "${AEGIS_CMAKE_OPTS[@]}" && cmake --build --preset asan-ubsan && ctest --preset asan-ubsan

python3 tools/run_test_layers.py --python .venv/bin/python   # each layer, reported separately
```

The bindings tests need `build/debug` to exist. They **fail** rather than skip
when it does not: a skipped test is not evidence (AEGIS-003).

Everything above is the canonical procedure and is what the `reproducibility`
job re-runs on a clean machine. One more command runs the *whole* gate matrix
locally in a single shot:

```bash
bash scripts/ci_local.sh
```

It is a convenience, not an extra requirement: CI runs those same stages as
separate jobs, so the `reproducibility` job does not invoke it a second time.

### Why the two CMake options

`-DPython3_EXECUTABLE` pins CMake to the virtual environment that actually has
pybind11 installed. Without it, CMake's `FindPython3` can select a different
interpreter — on a GitHub runner `actions/setup-python` exports
`Python3_ROOT_DIR`, which outranks `PATH`, so CMake picked the tool-cache
interpreter, found no pybind11 there, and skipped the bindings module.

`-DAEGIS_REQUIRE_BINDINGS=ON` makes that skip a configure-time error instead of
a silent one. It defaults to `OFF` so a machine with no Python development
headers can still build the rest of the project; the canonical procedure turns
it on because it has just installed those headers in step 0, so a skip there
means something is genuinely wrong. A silently missing bindings target also
leaves `cpp/bindings/aegis_module.cpp` out of `compile_commands.json`, which
makes clang-tidy analyse it with fallback flags and report a cascade of
diagnostics about the broken parse rather than about the code.

## Offline builds

`FetchContent` downloads GoogleTest and nlohmann/json, both pinned by archive
hash. To build without network access, populate a cache once and then point at it:

```bash
export FETCHCONTENT_BASE_DIR="$HOME/.cache/aegis-deps"
cmake --preset debug                      # populates the cache
cmake --preset debug -DFETCHCONTENT_FULLY_DISCONNECTED=ON
```

The hash pins mean an offline cache cannot silently be a different version from
the online one.

## Dependency changes

```bash
# 1. Edit requirements/python-requirements.in (pinned with ==).
# 2. Prove every pin is installable on every supported interpreter.
python3 tools/probe_dependencies.py --write-evidence
# 3. Re-resolve and re-hash.
bash scripts/lock_python.sh
# 4. Reinstall and re-verify.
.venv/bin/pip install --require-hashes -r requirements/requirements.lock
bash scripts/check_environment.sh
```

Step 2 exists because "it installed on my machine" is not the same claim as "it
installs on the interpreter CI uses", and the difference is discovered at the
worst possible moment otherwise.

## Container

[docker/Dockerfile.dev](../docker/Dockerfile.dev) packages the same environment
as a convenience. **It has still never been built** — Docker Desktop WSL
integration is disabled on the development host — and it is deliberately *not*
part of the canonical procedure above.

That is a scope statement, not an omission. AEGIS-009's frozen acceptance is
"clean environment instructions build and run integrity tests without
undocumented steps"; it names no container, and the canonical procedure is
already exercised on a clean runner by the `reproducibility` CI job. Building
the image would prove a second, redundant path. Until it is built, nothing may
claim the container as a reproducibility guarantee — see
[docs/LIMITATIONS.md](LIMITATIONS.md).

## Environment capture

```bash
python3 tools/capture_environment.py --preset release --output experiments/evidence/AEGIS-009/environment.json
```

The `reproducibility` CI job runs the same tool with `--preset debug` on a clean
GitHub-hosted runner and uploads the record as the `aegis-009-clean-machine`
artifact. The record carries a `ci` block naming the run, commit and runner
image, so an environment claim is traceable to the run that produced it.

Both halves of AEGIS-009's evidence live in `experiments/evidence/AEGIS-009/`:
`clean_machine_environment.json` is that artifact verbatim, and
`clean_machine_procedure.json` is the transcript — the commands the runner
actually echoed, its per-step outcomes and its result lines — captured from the
job log with the `gh` commands its own provenance block records.
`environment.json` is kept as the development-host record and is not the
clean-machine evidence.

The record covers the fields [docs/BENCHMARK_POLICY.md](BENCHMARK_POLICY.md)
requires: CPU model and count, memory, OS and kernel, virtualisation status,
compiler and version, build type, and the assertion and sanitizer state read out
of the compiled binary rather than described. Collecting these by hand when a
benchmark is written is how disclosure quietly becomes approximate.

## Reproducibility ledger

| Layer | Pinned by | Verified by |
|---|---|---|
| Python interpreter | `requires-python` in `pyproject.toml`, CI matrix | `scripts/check_environment.sh` |
| Python packages | `requirements/requirements.lock` with sha256 per file | `pip install --require-hashes`, dry-run drift check |
| C++ third party | archive URL + SHA-256 in CMake | CMake fails on hash mismatch |
| C++ toolchain | preset-selected compiler, recorded in `build_info()` | `tools/capture_environment.py` |
| Repository state | commit + dirty flag | `experiment_manifest`, `capture_environment` |
| Specification | `requirements/frozen_hashes.json` | `tools/check_frozen.py` |
