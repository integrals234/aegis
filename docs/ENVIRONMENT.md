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

```bash
git clone <repository> aegis && cd aegis

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

`clang-format` and `clang-tidy` are installed **from the Python lockfile**, not
from the distribution. That is deliberate: the version that gates CI is then
exactly the version a developer runs, and a static-analysis finding cannot depend
on whose machine ran it.

## Building and testing

```bash
export PATH="$PWD/.venv/bin:$PATH"

cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan && ctest --preset asan-ubsan

python3 tools/run_test_layers.py --python .venv/bin/python   # each layer, reported separately
bash scripts/ci_local.sh                                      # the whole gate matrix
```

The bindings tests need `build/debug` to exist. They **fail** rather than skip
when it does not: a skipped test is not evidence (AEGIS-003).

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

[docker/Dockerfile.dev](../docker/Dockerfile.dev) builds the same environment.
**It has not been built on the M0 development host** — Docker Desktop WSL
integration is disabled there — so M0 closes on local-virtualenv evidence and
AEGIS-009 carries a registered obligation for a clean-machine transcript. See
[docs/LIMITATIONS.md](LIMITATIONS.md).

## Environment capture

```bash
python3 tools/capture_environment.py --preset release --output experiments/evidence/AEGIS-009/environment.json
```

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
