#!/usr/bin/env bash
# Install the tracked git hooks into .git/hooks (AEGIS-001, AEGIS-010).
# Git hooks are not versioned by git itself, so they are kept in
# scripts/git-hooks/ and copied here. CI runs the same checks independently:
# a hook a contributor forgot to install must never be the only gate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOK_DIR="$(git -C "$ROOT" rev-parse --git-path hooks)"
mkdir -p "$HOOK_DIR"

for hook in "$ROOT"/scripts/git-hooks/*; do
  name="$(basename "$hook")"
  install -m 0755 "$hook" "$HOOK_DIR/$name"
  echo "installed $name -> $HOOK_DIR/$name"
done
