#!/usr/bin/env python3
"""Fail if a branch edits paths outside its active milestone's scope (AEGIS-007).

The acceptance criterion has two halves and both are checked here:

1. ``docs/BUILD_STATE.md`` identifies exactly one active milestone;
2. edits outside that milestone's permitted scope are detected unless documented.

Scope leakage is the failure mode that makes a milestone report untrue without
anybody lying: exchange code appears during M0, the milestone closes, and the
requirement that owns that code is never actually reviewed.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SCOPE = "configs/milestone_scope.yaml"
BUILD_STATE = "docs/BUILD_STATE.md"
ACTIVE_PREFIX = "- Active milestone:"
APPROVAL_PREFIX = "- Owner-approved scope changes:"


def read_build_state(root: Path) -> tuple[str | None, set[str], list[str]]:
    """Return (active milestone, owner-approved paths, errors)."""
    errors: list[str] = []
    path = root / BUILD_STATE
    if not path.exists():
        return None, set(), [f"{BUILD_STATE} is missing"]

    text = path.read_text(encoding="utf-8")
    actives = [line for line in text.splitlines() if line.startswith(ACTIVE_PREFIX)]
    if not actives:
        errors.append(f"{BUILD_STATE} does not name an active milestone")
        return None, set(), errors
    if len(actives) > 1:
        errors.append(f"{BUILD_STATE} names more than one active milestone: {actives}")
        return None, set(), errors

    active = actives[0][len(ACTIVE_PREFIX) :].strip()
    if not re.fullmatch(r"M[0-9]", active):
        errors.append(f"{BUILD_STATE} active milestone is not a milestone ID: {active!r}")
        return None, set(), errors

    approvals: set[str] = set()
    for line in text.splitlines():
        if line.startswith(APPROVAL_PREFIX):
            value = line[len(APPROVAL_PREFIX) :].strip()
            if value.lower() not in ("none", "none.", ""):
                approvals = {part.strip().strip("`") for part in value.split(",") if part.strip()}
    return active, approvals, errors


def git(root: Path, *args: str) -> str:
    result = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"ERROR: git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def changed_paths(root: Path, base: str) -> list[str]:
    merge_base = git(root, "merge-base", base, "HEAD").strip()
    parts = [
        git(root, "diff", "--name-only", merge_base, "HEAD"),
        git(root, "diff", "--name-only", "--cached"),
        git(root, "diff", "--name-only"),
    ]
    return sorted({line for part in parts for line in part.splitlines() if line})


def matches(path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)


def check(root: Path, scope_path: Path, base: str, paths: list[str] | None = None) -> list[str]:
    doc = yaml.safe_load(scope_path.read_text(encoding="utf-8"))
    active, approvals, errors = read_build_state(root)
    if active is None:
        return errors

    milestone = doc["milestones"].get(active)
    if milestone is None:
        return [*errors, f"{scope_path.name} declares no scope for active milestone {active}"]

    always = list(doc.get("always_allowed", []))
    allowed = [*always, *milestone.get("allowed", [])]
    denied = list(milestone.get("denied", []))
    changed = paths if paths is not None else changed_paths(root, base)

    for path in changed:
        if matches(path, sorted(approvals)) or path in approvals:
            continue
        # always_allowed and owner approvals outrank the milestone's deny list:
        # they describe paths no milestone owns (governance state, hygiene).
        if matches(path, always):
            continue
        if matches(path, denied):
            errors.append(
                f"{path} belongs to a later milestone and is denied while {active} is active"
            )
        elif not matches(path, allowed):
            errors.append(
                f"{path} is outside the permitted scope of {active}; "
                f"record an owner approval on the '{APPROVAL_PREFIX}' line of {BUILD_STATE} if intended"
            )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--scope", type=Path)
    parser.add_argument("--base", default="main")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    errors = check(root, args.scope or root / DEFAULT_SCOPE, args.base)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 2
    active, _, _ = read_build_state(root)
    print(f"Scope guard passed: every changed path is within the scope of {active}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
