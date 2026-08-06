#!/usr/bin/env python3
"""Check that documentation is present, current and resolvable (AEGIS-235).

The acceptance criterion is "documentation audit maps claims to code/evidence",
so this tool is executable rather than a checklist:

* required documents exist and contain more than a heading;
* every ``evidence:`` marker names a path that exists;
* every relative Markdown link into the repository resolves;
* documents that name a requirement ID name one that exists.

What it deliberately does not do is judge whether prose is accurate. That is the
spec-auditor's job; this tool removes the mechanical failures — a dead link, a
deleted evidence file, a runbook that was never written — so review time is
spent on the part that needs judgement.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from audit_requirements import _is_substantive, load_json

ROOT = Path(__file__).resolve().parents[1]

REQUIRED_DOCS = (
    "docs/ARCHITECTURE.md",
    "docs/ROADMAP.md",
    "docs/BUILD_STATE.md",
    "docs/ACCEPTANCE_GATES.md",
    "docs/ENVIRONMENT.md",
    "docs/RUNBOOK.md",
    "docs/DEMO.md",
    "docs/LIMITATIONS.md",
    "docs/RECOVERY_CONTRACT.md",
    "docs/TRACEABILITY_MATRIX.md",
)

SCAN_GLOBS = ("*.md", "docs/**/*.md", "adr/**/*.md", "experiments/**/*.md")

EVIDENCE_MARKER = re.compile(r"evidence:\s*([^\s,;)\]]+)", re.IGNORECASE)
MD_LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
REQUIREMENT_ID = re.compile(r"\bAEGIS-(\d{3})\b")

EXTERNAL = ("http://", "https://", "mailto:", "#")


def scan_files(root: Path) -> list[Path]:
    files: set[Path] = set()
    for pattern in SCAN_GLOBS:
        files.update(p for p in root.glob(pattern) if p.is_file() and ".venv" not in p.parts)
    return sorted(files)


def run(root: Path) -> list[str]:
    errors: list[str] = []

    for rel in REQUIRED_DOCS:
        path = root / rel
        if not path.exists():
            errors.append(f"required document missing: {rel}")
            continue
        good, why = _is_substantive(path)
        if not good:
            errors.append(f"required document {rel} {why}")

    valid_ids = {r["id"] for r in load_json(root / "requirements/requirements.json")["requirements"]}

    for path in scan_files(root):
        rel = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")

        for lineno, line in enumerate(text.splitlines(), start=1):
            for marker in EVIDENCE_MARKER.findall(line):
                target = marker.strip("`'\"")
                if not (root / target).exists():
                    errors.append(f"{rel}:{lineno}: evidence path does not exist: {target}")

            for link in MD_LINK.findall(line):
                target = link.split("#", 1)[0].strip()
                if not target or target.startswith(EXTERNAL):
                    continue
                resolved = (path.parent / target) if not target.startswith("/") else (root / target.lstrip("/"))
                # Links in AEGIS docs are written relative to the repository root
                # so that they work both on disk and in the VS Code panel.
                if not resolved.exists() and not (root / target).exists():
                    errors.append(f"{rel}:{lineno}: broken link: {target}")

            for number in REQUIREMENT_ID.findall(line):
                if f"AEGIS-{number}" not in valid_ids:
                    errors.append(f"{rel}:{lineno}: references unknown requirement AEGIS-{number}")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    errors = run(root)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print(f"Documentation audit failed: {len(errors)} problem(s)", file=sys.stderr)
        return 2
    print(f"Documentation audit passed: {len(REQUIRED_DOCS)} required documents present and resolvable")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
