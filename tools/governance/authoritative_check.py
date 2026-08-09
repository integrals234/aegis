#!/usr/bin/env python3
"""The authoritative governance gate (R8, AEGIS-007, AEGIS-001; ADR-0014).

**This file is only authoritative when it is the copy on protected `main`.**
``.github/workflows/governance.yml`` runs it under ``pull_request_target``, so
both the workflow definition and this checker come from the base branch. A pull
request may rewrite its own copy of this file and change nothing about the
verdict it receives.

That is the fix for R8. Before it, every authority surface lived in the branch
being judged:

1. ``docs/BUILD_STATE.md``'s ``- Owner-approved scope changes:`` line, which
   ``tools/check_scope.py`` honoured — so a branch could authorise its own
   out-of-scope edit;
2. the *same* line, which ``tools/check_frozen.py`` also honoured — so a branch
   could edit a frozen specification file *and* its recorded digest, and pass
   both the content and the history check;
3. ``configs/milestone_scope.yaml``, which every milestone allows editing — so
   a branch could widen its own ``allowed`` list;
4. the active-milestone line, where ``M9`` names an empty ``denied`` list and a
   tree-wide ``allowed`` list.

All four are closed by reading policy from the base branch instead. What the
candidate says about itself is evidence, never authority.

Six checks, every one fail-closed. Absence of governance data is a failure, not
a default:

1. every changed path lies inside the active milestone's declared scope;
2. a changed governance path carries an exact-path owner approval;
3. every frozen file still hashes to the digest recorded on ``main``;
4. a changed frozen path carries an exact-path owner approval;
5. the candidate's ``docs/BUILD_STATE.md`` names the same active milestone as
   the trusted policy;
6. the trusted policy is present, parseable and of a known version.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

# Loaded via the sys.path insert above so this file runs as a script from any
# working directory.
from readers import (
    CandidateReader,
    DirectoryReader,
    GitHubApiReader,
    GitShowReader,
)

ROOT = Path(__file__).resolve().parents[2]

POLICY = "configs/governance/policy.yaml"
SCOPE = "configs/milestone_scope.yaml"
FROZEN_MANIFEST = "requirements/frozen_hashes.json"
BUILD_STATE = "docs/BUILD_STATE.md"
ACTIVE_PREFIX = "- Active milestone:"
SUPPORTED_POLICY_VERSIONS = {1}


class PolicyError(RuntimeError):
    """The trusted policy could not be established. Always fatal."""


# --------------------------------------------------------------------- policy


def load_policy(trusted_root: Path) -> dict[str, Any]:
    path = trusted_root / POLICY
    if not path.exists():
        raise PolicyError(f"{POLICY} is missing from the trusted tree")
    try:
        document = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as error:
        raise PolicyError(f"{POLICY} is not parseable: {error}") from error
    if not isinstance(document, dict):
        raise PolicyError(f"{POLICY} must be a mapping")

    version = document.get("policy_version")
    if version not in SUPPORTED_POLICY_VERSIONS:
        raise PolicyError(
            f"{POLICY} declares policy_version {version!r}; this checker understands "
            f"{sorted(SUPPORTED_POLICY_VERSIONS)}. Refusing to guess."
        )

    active = document.get("active_milestone")
    if not isinstance(active, str) or not re.fullmatch(r"M[0-9]", active):
        raise PolicyError(f"{POLICY} active_milestone is not a milestone ID: {active!r}")

    # Absent or null means "no approvals". Anything else that is not a list is a
    # malformed policy and must be loud: `approvals: {}` is falsy, so treating it
    # as empty would silently ignore a mapping somebody meant to grant something.
    approvals = document.get("approvals")
    if approvals is None:
        approvals = []
    if not isinstance(approvals, list):
        raise PolicyError(
            f"{POLICY} approvals must be a list, got {type(approvals).__name__}"
        )
    for entry in approvals:
        if not isinstance(entry, dict):
            raise PolicyError(f"{POLICY} approval entry is not a mapping: {entry!r}")
        for field in ("id", "milestone", "paths", "reason"):
            if field not in entry:
                raise PolicyError(f"{POLICY} approval {entry.get('id', '?')!r} lacks {field!r}")
        for candidate_path in entry["paths"]:
            if any(ch in candidate_path for ch in "*?["):
                raise PolicyError(
                    f"{POLICY} approval {entry['id']!r} uses a glob ({candidate_path!r}); "
                    "approvals must name exact paths so they cannot widen beyond review"
                )
    return document


def approved_paths(policy: dict[str, Any]) -> dict[str, str]:
    """Exact paths approved *for the active milestone*, mapped to approval id.

    An approval for another milestone is inert, so a permission granted for M2
    cannot silently keep working through M3.
    """
    active = policy["active_milestone"]
    granted: dict[str, str] = {}
    for entry in policy.get("approvals") or []:  # validated in load_policy()
        if entry.get("milestone") != active:
            continue
        for path in entry["paths"]:
            granted[path] = entry["id"]
    return granted


# ---------------------------------------------------------------------- input


def load_scope(trusted_root: Path) -> dict[str, Any]:
    path = trusted_root / SCOPE
    if not path.exists():
        raise PolicyError(f"{SCOPE} is missing from the trusted tree")
    document = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or "milestones" not in document:
        raise PolicyError(f"{SCOPE} has no milestones block")
    return document


def load_frozen(trusted_root: Path) -> dict[str, str]:
    path = trusted_root / FROZEN_MANIFEST
    if not path.exists():
        raise PolicyError(f"{FROZEN_MANIFEST} is missing from the trusted tree")
    manifest: dict[str, str] = json.loads(path.read_text(encoding="utf-8"))
    return manifest


def matches(path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)


# --------------------------------------------------------------------- checks


def _check_scope(
    changed: list[str],
    scope: dict[str, Any],
    active: str,
    granted: dict[str, str],
    governance_paths: list[str],
    self_path: str,
    errors: list[str],
) -> None:
    milestone = scope["milestones"].get(active)
    if milestone is None:
        raise PolicyError(f"{SCOPE} declares no scope for active milestone {active}")

    always = list(scope.get("always_allowed", []))
    allowed = [*always, *milestone.get("allowed", [])]
    denied = list(milestone.get("denied", []))

    for path in changed:
        if path in granted or path == self_path:
            continue
        if matches(path, always):
            continue
        if matches(path, denied):
            errors.append(
                f"{path}: belongs to a later milestone and is denied while {active} is active"
            )
        elif not matches(path, allowed):
            errors.append(
                f"{path}: outside the permitted scope of {active}. An owner approval naming "
                f"this exact path must be merged into {POLICY} on main first."
            )


def _check_governance_paths(
    changed: list[str],
    governance_paths: list[str],
    granted: dict[str, str],
    self_path: str,
    errors: list[str],
) -> None:
    for path in changed:
        if path == self_path:
            # Inert until merged; merging is the owner's act. See policy.yaml.
            continue
        if matches(path, governance_paths) and path not in granted:
            errors.append(
                f"{path}: changes who is allowed to change what, so it needs an owner approval "
                f"naming this exact path in {POLICY} on main. Tampering with the local checkers "
                "does not affect this verdict — the gate runs main's copy."
            )


def _check_frozen(
    changed: list[str],
    frozen: dict[str, str],
    reader: CandidateReader,
    granted: dict[str, str],
    errors: list[str],
) -> None:
    protected = set(frozen) | {FROZEN_MANIFEST}

    for path in changed:
        if path in protected and path not in granted:
            errors.append(
                f"{path}: frozen specification path modified without an owner approval in "
                f"{POLICY} on main. Rewriting the candidate's own {FROZEN_MANIFEST} does not "
                "help: digests are compared against main's copy."
            )

    for path, expected in sorted(frozen.items()):
        if path in granted:
            continue
        content = reader.read_text(path)
        if content is None:
            errors.append(f"{path}: frozen file is missing from the candidate revision")
            continue
        actual = hashlib.sha256(content.encode("utf-8")).hexdigest()
        if actual != expected:
            errors.append(
                f"{path}: frozen file content differs from the digest recorded on main\n"
                f"    main   sha256 {expected}\n"
                f"    branch sha256 {actual}"
            )


def _check_build_state(
    reader: CandidateReader, active: str, errors: list[str]
) -> None:
    content = reader.read_text(BUILD_STATE)
    if content is None:
        errors.append(f"{BUILD_STATE}: missing from the candidate revision")
        return
    declared = [line for line in content.splitlines() if line.startswith(ACTIVE_PREFIX)]
    if len(declared) != 1:
        errors.append(
            f"{BUILD_STATE}: must name exactly one active milestone, found {len(declared)}"
        )
        return
    value = declared[0][len(ACTIVE_PREFIX) :].strip()
    if value != active:
        errors.append(
            f"{BUILD_STATE}: names active milestone {value!r} but the trusted policy on main "
            f"says {active!r}. The milestone is set by merging a policy change, not by editing "
            "this file."
        )


# ------------------------------------------------------------------- entry


def run(trusted_root: Path, reader: CandidateReader, changed_files: list[str]) -> list[str]:
    """Judge a candidate revision. Returns a list of errors; empty means pass.

    ``trusted_root`` must be a checkout of the base branch. Passing the
    candidate's own tree here would reintroduce exactly the circularity this
    module exists to remove.
    """
    policy = load_policy(trusted_root)
    scope = load_scope(trusted_root)
    frozen = load_frozen(trusted_root)

    active = policy["active_milestone"]
    granted = approved_paths(policy)
    governance_paths = list(policy.get("governance_paths") or [])
    self_path = policy.get("self_path", POLICY)
    changed = sorted(set(changed_files))

    errors: list[str] = []
    _check_build_state(reader, active, errors)
    _check_governance_paths(changed, governance_paths, granted, self_path, errors)
    _check_frozen(changed, frozen, reader, granted, errors)
    _check_scope(changed, scope, active, granted, governance_paths, self_path, errors)
    return errors


def _build_reader(args: argparse.Namespace) -> tuple[CandidateReader, list[str]]:
    if args.repo:
        reader = GitHubApiReader(args.repo, args.head_sha, args.pr)
        return reader, list(reader.changed_files())
    if args.candidate_dir:
        if not args.changed_files_from:
            raise PolicyError("--candidate-dir requires --changed-files-from")
        listing = Path(args.changed_files_from).read_text(encoding="utf-8")
        changed = [line.strip() for line in listing.splitlines() if line.strip()]
        return DirectoryReader(str(args.candidate_dir)), changed
    local = GitShowReader(str(args.candidate_repo), args.head_sha)
    return local, local.changed_files(args.base)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trusted-root", type=Path, default=ROOT,
                        help="checkout of the BASE branch; never the candidate's own tree")
    parser.add_argument("--repo", help="OWNER/NAME — read the candidate through the GitHub API")
    parser.add_argument("--pr", type=int, help="pull request number (GitHub mode)")
    parser.add_argument("--head-sha", default="HEAD", help="candidate revision")
    parser.add_argument("--candidate-repo", type=Path, default=Path.cwd(),
                        help="local repository to read the candidate from (local mode)")
    parser.add_argument("--base", default="origin/main", help="base ref (local mode)")
    parser.add_argument("--candidate-dir", type=Path,
                        help="read the candidate from a plain directory (fixture mode)")
    parser.add_argument("--changed-files-from", type=Path,
                        help="file listing the candidate's changed paths (fixture mode)")
    args = parser.parse_args(argv)

    if args.repo and args.pr is None:
        parser.error("--repo requires --pr")

    try:
        reader, changed = _build_reader(args)
        errors = run(args.trusted_root.resolve(), reader, changed)
    except (PolicyError, RuntimeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        print("Authoritative governance gate: FAILED (closed)", file=sys.stderr)
        return 2

    print(f"Candidate changes {len(changed)} path(s).")
    sys.stdout.flush()
    if errors:
        for message in errors:
            print(f"ERROR: {message}", file=sys.stderr)
        print("\nAuthoritative governance gate: FAILED", file=sys.stderr)
        return 2
    print("Authoritative governance gate passed: every changed path is authorised by "
          "the policy on main.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
