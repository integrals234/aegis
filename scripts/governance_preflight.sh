#!/usr/bin/env bash
# Run main's governance gate against the current branch, before opening a PR.
#
# The authoritative verdict comes from the "Authoritative governance gate (R8)"
# job, which runs the checker from protected `main`. This script reproduces that
# verdict locally by doing the same thing: it materialises `origin/main` in a
# throwaway worktree and runs MAIN'S checker — not this branch's copy — against
# this branch's diff.
#
# That distinction is the point. `tools/check_scope.py` and
# `tools/check_frozen.py` are advisory and live in the branch, so a branch can
# make them say anything. This script borrows the trusted copy instead, which
# is why its answer matches CI.
#
# It is still only convenience: an agent can edit this script too. Doing so
# changes local feedback and nothing about the verdict a pull request receives.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BASE="${AEGIS_BASE_REF:-origin/main}"
PYTHON="${AEGIS_PYTHON:-$ROOT/.venv/bin/python}"
[[ -x "$PYTHON" ]] || PYTHON="python3"

echo "Fetching $BASE ..."
git fetch --quiet origin main

WORKTREE="$(mktemp -d)"
cleanup() { git worktree remove --force "$WORKTREE" >/dev/null 2>&1 || true; rm -rf "$WORKTREE"; }
trap cleanup EXIT

# --detach: we only want main's files, never to move a branch ref.
git worktree add --quiet --detach "$WORKTREE" "$BASE"

CHECKER="$WORKTREE/tools/governance/authoritative_check.py"
if [[ ! -f "$CHECKER" ]]; then
  cat >&2 <<MSG
ERROR: $BASE has no tools/governance/authoritative_check.py.

The authoritative checker must come from the base branch — that is the entire
point, and borrowing this branch's copy instead would reproduce exactly the
circularity R8 removed. So this script cannot fall back to a local copy.

If the R8 bootstrap has not merged yet, this is expected: there is no
authoritative gate to preflight against, and the bootstrap pull request is
judged by the pre-existing checks plus owner review. See ADR-0014.
MSG
  exit 2
fi

echo "Judging $(git rev-parse --short HEAD) against the policy on $BASE"
echo

"$PYTHON" "$CHECKER" \
  --trusted-root "$WORKTREE" \
  --candidate-repo "$ROOT" \
  --head-sha HEAD \
  --base "$BASE"
