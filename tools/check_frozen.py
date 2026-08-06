#!/usr/bin/env python3
"""Fail if a frozen specification file was modified without owner approval.

AEGIS-001. The PreToolUse hook in ``.claude/hooks/protect_spec.py`` is defence in
depth: it only sees writes made through Claude's file tools, so anything routed
through a shell bypasses it entirely. This checker is the control of record. It
runs on the diff, in pre-commit and in CI, and it answers a question the hook
cannot: *did the branch change a frozen file at any point?*

Two independent conditions are checked:

1. **Content** — every path in ``requirements/frozen_hashes.json`` still hashes
   to its recorded digest.
2. **History** — no frozen path appears in the diff against the base ref.

The only legitimate way past (2) is an owner approval recorded in
``docs/BUILD_STATE.md``, which Claude must not write on its own behalf.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = "requirements/frozen_hashes.json"
BUILD_STATE = "docs/BUILD_STATE.md"
APPROVAL_PREFIX = "- Owner-approved scope changes:"

# Changing these does not corrupt the specification, but it changes who is
# allowed to change the specification. A diff that touches them is reported so
# the owner sees it in review; it is not by itself a failure, because M0 has to
# build this machinery in the first place.
GOVERNANCE_PATHS = (
    ".claude/settings.json",
    ".claude/hooks/protect_spec.py",
    "tools/check_frozen.py",
    "tools/audit_requirements.py",
)


def frozen_paths(root: Path) -> dict[str, str]:
    manifest = root / MANIFEST
    if not manifest.exists():
        raise SystemExit(f"ERROR: frozen manifest missing: {MANIFEST}")
    paths: dict[str, str] = json.loads(manifest.read_text(encoding="utf-8"))
    return paths


def approved_paths(root: Path) -> set[str]:
    """Parse owner approvals out of docs/BUILD_STATE.md."""
    state = root / BUILD_STATE
    if not state.exists():
        return set()
    for line in state.read_text(encoding="utf-8").splitlines():
        if line.startswith(APPROVAL_PREFIX):
            value = line[len(APPROVAL_PREFIX) :].strip()
            if value.lower() in ("none", "none.", ""):
                return set()
            return {part.strip().strip("`") for part in value.split(",") if part.strip()}
    return set()


def git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=root, capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise SystemExit(f"ERROR: git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def changed_paths(root: Path, base: str) -> list[str]:
    merge_base = git(root, "merge-base", base, "HEAD").strip()
    diff = git(root, "diff", "--name-only", merge_base, "HEAD")
    staged = git(root, "diff", "--name-only", "--cached")
    unstaged = git(root, "diff", "--name-only")
    return sorted({line for line in (diff + staged + unstaged).splitlines() if line})


def check_content(root: Path, manifest: dict[str, str]) -> list[str]:
    errors = []
    for rel, expected in manifest.items():
        path = root / rel
        if not path.exists():
            errors.append(f"frozen file missing: {rel}")
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            errors.append(
                f"frozen file content changed: {rel}\n"
                f"    expected sha256 {expected}\n"
                f"    actual   sha256 {actual}"
            )
    return errors


def check_history(root: Path, manifest: dict[str, str], base: str, approvals: set[str]) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    notices: list[str] = []
    protected = set(manifest) | {MANIFEST}
    try:
        changed = changed_paths(root, base)
    except SystemExit as exc:
        notices.append(f"history check skipped: {exc}")
        return errors, notices

    for path in changed:
        if path in protected and path not in approvals:
            errors.append(
                f"frozen path modified on this branch without owner approval: {path} "
                f"(record the approval on the '{APPROVAL_PREFIX}' line of {BUILD_STATE})"
            )
        elif path in protected:
            notices.append(f"frozen path changed under recorded owner approval: {path}")
        elif path in GOVERNANCE_PATHS:
            notices.append(f"governance control changed on this branch (review required): {path}")
    return errors, notices


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--base", default="main", help="base ref for the history check")
    parser.add_argument("--no-history", action="store_true", help="check content only")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    manifest = frozen_paths(root)
    errors = check_content(root, manifest)
    notices: list[str] = []
    if not args.no_history:
        history_errors, notices = check_history(root, manifest, args.base, approved_paths(root))
        errors.extend(history_errors)

    for notice in notices:
        print(f"NOTICE: {notice}")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 2
    print(f"Frozen-file integrity check passed: {len(manifest)} frozen paths intact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
