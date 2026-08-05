#!/usr/bin/env bash
set -euo pipefail
missing=0
for cmd in git python3 cmake ninja clang clang++ ; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "MISSING: $cmd"
    missing=1
  else
    echo "OK: $cmd -> $(command -v "$cmd")"
  fi
done
python3 tools/audit_requirements.py --quick
exit "$missing"
