#!/usr/bin/env bash
# Verify the development environment matches docs/ENVIRONMENT.md (AEGIS-009).
#
# The original version checked only that a handful of commands existed. Presence
# is not the property that matters: a cmake too old to understand the presets, or
# a clang without C++20, produces a failure three steps later that looks like a
# code problem. So this checks *versions*, and it checks the tools the gates
# actually run — pytest, ruff, mypy, clang-format, clang-tidy — not only the
# compiler.
#
# Exit codes: 0 all good; 1 something is missing, too old, or drifted.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MISSING=0
: "${AEGIS_VENV:=$ROOT/.venv}"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }

# Returns 0 when $1 >= $2, comparing dotted versions.
version_at_least() {
  [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]
}

require_command() {
  local name="$1" minimum="${2:-}"
  local path version
  if ! path="$(command -v "$name" 2>/dev/null)"; then
    red "MISSING: $name"
    MISSING=1
    return
  fi
  if [ -z "$minimum" ]; then
    green "OK: $name -> $path"
    return
  fi
  version="$("$name" --version 2>&1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n1)"
  if [ -z "$version" ]; then
    red "UNKNOWN VERSION: $name at $path (expected >= $minimum)"
    MISSING=1
  elif version_at_least "$version" "$minimum"; then
    green "OK: $name $version >= $minimum -> $path"
  else
    red "TOO OLD: $name $version < $minimum -> $path"
    MISSING=1
  fi
}

echo "=== Build toolchain ==="
require_command git 2.30
require_command python3 3.12
# 3.24 is the floor the presets and this FetchContent usage require.
require_command cmake 3.24
require_command ninja 1.10
require_command clang++ 16.0
echo

echo "=== Pinned Python environment ==="
if [ ! -x "$AEGIS_VENV/bin/python" ]; then
  red "MISSING: $AEGIS_VENV"
  echo "         python3 -m venv .venv"
  echo "         .venv/bin/pip install --require-hashes -r requirements/requirements.lock"
  MISSING=1
else
  green "OK: virtual environment at $AEGIS_VENV"
  # The gates run these. A missing one means a gate silently does not run, which
  # is worse than a gate that fails.
  for tool in pytest ruff mypy clang-format clang-tidy; do
    if [ -x "$AEGIS_VENV/bin/$tool" ]; then
      green "OK: $tool $("$AEGIS_VENV/bin/$tool" --version 2>&1 | head -n1)"
    else
      red "MISSING: $AEGIS_VENV/bin/$tool (reinstall requirements/requirements.lock)"
      MISSING=1
    fi
  done

  echo
  echo "=== Lockfile integrity ==="
  # A dry-run install under --require-hashes proves the installed set still
  # satisfies the lock, which `pip list` cannot.
  if "$AEGIS_VENV/bin/pip" install --quiet --dry-run --require-hashes \
       -r requirements/requirements.lock >/dev/null 2>&1; then
    green "OK: installed packages satisfy requirements/requirements.lock"
  else
    red "DRIFT: the environment no longer matches requirements/requirements.lock"
    echo "       .venv/bin/pip install --require-hashes -r requirements/requirements.lock"
    MISSING=1
  fi
fi
echo

echo "=== Repository integrity ==="
python3 tools/audit_requirements.py --quick || MISSING=1
python3 tools/check_frozen.py --no-history || MISSING=1
echo

if [ "$MISSING" -ne 0 ]; then
  red "Environment check FAILED. See docs/ENVIRONMENT.md."
else
  green "Environment check passed."
fi
exit "$MISSING"
