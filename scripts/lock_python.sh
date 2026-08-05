#!/usr/bin/env bash
# Regenerate requirements/requirements.lock from requirements/python-requirements.in.
# AEGIS-228. Resolution happens in a throwaway virtual environment so the lock can
# never pick up whatever happens to be installed on the developer's machine.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${AEGIS_PYTHON:-python3}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "Resolving with $("$PYTHON" -V) in $WORKDIR"
"$PYTHON" -m venv "$WORKDIR/venv"
"$WORKDIR/venv/bin/pip" install --quiet --upgrade pip
"$WORKDIR/venv/bin/pip" install --quiet -r "$ROOT/requirements/python-requirements.in"
"$WORKDIR/venv/bin/pip" freeze --exclude-editable > "$WORKDIR/freeze.txt"

"$PYTHON" "$ROOT/tools/generate_lockfile.py" \
  --input "$WORKDIR/freeze.txt" \
  --output "$ROOT/requirements/requirements.lock"
