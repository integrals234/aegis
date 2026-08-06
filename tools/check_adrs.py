#!/usr/bin/env python3
"""Validate architecture decision records and their linkage (AEGIS-008).

Two halves, matching the acceptance criterion "each nontrivial architecture
change references an accepted ADR":

* **Structure** — every ADR carries the template's sections, a recognised
  status, a date, a milestone and requirement IDs that exist in the catalogue;
  no document references an ADR number that was never written.
* **Linkage** — if the branch changes a path listed in ``architectural_paths``,
  at least one commit message on the branch (or the milestone report) must cite
  an ADR. Without this half, ADRs stay a folder nobody is obliged to update.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from audit_requirements import load_json

ROOT = Path(__file__).resolve().parents[1]
ADR_DIR = "adr"
TEMPLATE = "0000-template.md"

REQUIRED_SECTIONS = (
    "## Context",
    "## Decision",
    "## Alternatives considered",
    "## Consequences",
    "## Verification",
)
VALID_STATUS = {"Proposed", "Accepted", "Rejected", "Superseded", "Deprecated"}
ADR_REFERENCE = re.compile(r"\bADR-(\d{4})\b")
ADR_FILENAME = re.compile(r"^(\d{4})-[a-z0-9-]+\.md$")

# A change under one of these paths is an architecture change, not a tweak.
ARCHITECTURAL_PATHS = (
    "cpp/**",
    "python/common/**",
    "configs/architecture_rules.yaml",
    "configs/schemas/**",
    "docs/ARCHITECTURE.md",
)
REFERENCE_DOCS = ("docs", "adr", "experiments")


def git(root: Path, *args: str) -> str:
    result = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"ERROR: git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def field(text: str, name: str) -> str | None:
    match = re.search(rf"^-\s*{re.escape(name)}\s*:\s*(.*)$", text, re.MULTILINE)
    return match.group(1).strip() if match else None


def check_structure(root: Path) -> tuple[list[str], set[str]]:
    errors: list[str] = []
    known: set[str] = set()
    adr_dir = root / ADR_DIR
    if not adr_dir.exists():
        return [f"{ADR_DIR}/ is missing"], known

    catalogue = load_json(root / "requirements/requirements.json")["requirements"]
    valid_requirements = {r["id"] for r in catalogue}

    for path in sorted(adr_dir.glob("*.md")):
        name = path.name
        if name == TEMPLATE:
            continue
        rel = f"{ADR_DIR}/{name}"
        if not ADR_FILENAME.match(name):
            errors.append(f"{rel}: filename must be NNNN-kebab-case-title.md")
            continue
        matched = ADR_FILENAME.match(name)
        assert matched is not None  # guarded by the branch above
        known.add(matched.group(1))

        text = path.read_text(encoding="utf-8")
        for section in REQUIRED_SECTIONS:
            if section not in text:
                errors.append(f"{rel}: missing required section '{section}'")

        status = field(text, "Status")
        if status is None:
            errors.append(f"{rel}: no 'Status:' field")
        elif status not in VALID_STATUS:
            errors.append(f"{rel}: status {status!r} is not one of {sorted(VALID_STATUS)}")

        if not field(text, "Date"):
            errors.append(f"{rel}: no 'Date:' field")
        milestone = field(text, "Milestone")
        if not milestone or not re.fullmatch(r"M[0-9]", milestone):
            errors.append(f"{rel}: 'Milestone:' must be a milestone ID, got {milestone!r}")

        ids_field = field(text, "Requirement IDs") or ""
        ids = re.findall(r"AEGIS-\d{3}", ids_field)
        if not ids:
            errors.append(f"{rel}: 'Requirement IDs:' must cite at least one requirement")
        for rid in ids:
            if rid not in valid_requirements:
                errors.append(f"{rel}: cites unknown requirement {rid}")

    return errors, known


def check_dangling_references(root: Path, known: set[str]) -> list[str]:
    errors: list[str] = []
    for folder in REFERENCE_DOCS:
        base = root / folder
        if not base.exists():
            continue
        for path in sorted(base.rglob("*.md")):
            rel = path.relative_to(root).as_posix()
            for number in set(ADR_REFERENCE.findall(path.read_text(encoding="utf-8"))):
                if number == "0000":
                    continue
                if number not in known:
                    errors.append(f"{rel}: references ADR-{number}, which does not exist")
    return errors


def check_linkage(root: Path, base_ref: str) -> list[str]:
    """An architecture change on the branch must cite an ADR somewhere."""
    try:
        merge_base = git(root, "merge-base", base_ref, "HEAD").strip()
    except SystemExit:
        return []  # no base ref available (shallow clone, fresh repo)

    changed = [p for p in git(root, "diff", "--name-only", merge_base, "HEAD").splitlines() if p]
    architectural = [p for p in changed if any(fnmatch.fnmatch(p, g) for g in ARCHITECTURAL_PATHS)]
    if not architectural:
        return []

    messages = git(root, "log", "--format=%B", f"{merge_base}..HEAD")
    if ADR_REFERENCE.search(messages):
        return []
    reports = root / "experiments/milestone-reports"
    if reports.exists():
        for report in reports.glob("*.md"):
            if ADR_REFERENCE.search(report.read_text(encoding="utf-8")):
                return []
    return [
        "architecture paths changed on this branch without any ADR reference in the commit "
        f"messages or milestone report: {', '.join(sorted(architectural)[:5])}"
    ]


def run(root: Path, base_ref: str = "main", check_link: bool = True) -> list[str]:
    errors, known = check_structure(root)
    errors.extend(check_dangling_references(root, known))
    if check_link:
        errors.extend(check_linkage(root, base_ref))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--base", default="main")
    parser.add_argument("--no-linkage", action="store_true")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    errors = run(root, args.base, not args.no_linkage)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 2
    count = len([p for p in (root / ADR_DIR).glob("*.md") if p.name != TEMPLATE])
    print(f"ADR check passed: {count} decision record(s) valid and linked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
