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

echo "[style] clang-tidy -p $BUILD_DIR"
clang-tidy -p "$BUILD_DIR" --quiet "${SOURCES[@]}"

echo "[style] ok"
