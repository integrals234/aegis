#!/usr/bin/env bash
# clang-format and clang-tidy over every tracked C++ source (AEGIS-227).
# Both tools are pinned in requirements/requirements.lock, so the version that
# gates CI is the version a developer runs locally.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${1:-build/debug}"
# --others includes files staged for a first commit; a new source must be
# style-checked before it lands, not after.
mapfile -t SOURCES < <(git ls-files --cached --others --exclude-standard \
  'cpp/*.cpp' 'cpp/*.hpp' 'tests/cpp/*.cpp' 'tests/cpp/*.hpp')

if [[ ${#SOURCES[@]} -eq 0 ]]; then
  echo "No C++ sources tracked yet; nothing to check."
  exit 0
fi

echo "[style] clang-format --dry-run -Werror (${#SOURCES[@]} files)"
clang-format --dry-run -Werror "${SOURCES[@]}"

if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
  echo "ERROR: $BUILD_DIR/compile_commands.json is missing; run 'cmake --preset debug' first" >&2
  exit 2
fi

# AEGIS-227 (M5 addendum, m5-milestone-gate): parallelised over the exact
# same full-tree SOURCES list built above -- never changed-files-only, never
# narrowed. One clang-tidy invocation per file, fanned out across
# AEGIS_TIDY_JOBS workers (default: the machine's core count); xargs exits
# non-zero the moment any worker does, and `set -e` above turns that into
# this script failing, so a single file's diagnostic cannot be silently
# swallowed by the parallel fan-out. tests/unit/test_check_cpp_style_parallel.py
# proves both properties (full coverage, failure propagation) against a
# throwaway fixture repo.
JOBS="${AEGIS_TIDY_JOBS:-$(nproc)}"
echo "[style] clang-tidy -p $BUILD_DIR (parallel, $JOBS jobs, ${#SOURCES[@]} files)"
printf '%s\0' "${SOURCES[@]}" | xargs -0 -P "$JOBS" -n 1 clang-tidy -p "$BUILD_DIR" --quiet

echo "[style] ok"
