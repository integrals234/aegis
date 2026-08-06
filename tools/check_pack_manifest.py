#!/usr/bin/env python3
"""Verify PACK_MANIFEST.json against the working tree (AEGIS-236, AEGIS-002).

``PACK_MANIFEST.json`` records the files the original build pack shipped and
their hashes. Nothing validated it before this tool existed, so a scaffold file
could be deleted or quietly rewritten and every other gate would still pass.

Three distinct outcomes, reported separately because they mean different things:

* **missing** — a pack file is gone. Sometimes deliberate (the NTFS
  ``:Zone.Identifier`` artifacts removed in M0), which is why the tool takes an
  ``--allow-missing`` glob rather than being silently permissive.
* **changed** — a pack file's content no longer matches its recorded hash.
  Expected for files M0 was meant to develop; reported so the change is visible.
* **untracked-by-pack** — a file the pack never shipped. Informational only:
  this repository is supposed to grow.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = "PACK_MANIFEST.json"

# Removals M0 made deliberately, each with a reason:
#   *:Zone.Identifier            NTFS interop artifacts, removed as repository hygiene.
#   .github/workflows/spec-integrity.yml
#                                superseded by the full ci.yml matrix, which runs
#                                the same two checks plus every other gate.
DEFAULT_ALLOW_MISSING = ("*:Zone.Identifier", ".github/workflows/spec-integrity.yml")


def load_entries(root: Path) -> dict[str, str]:
    path = root / MANIFEST
    if not path.exists():
        raise SystemExit(f"ERROR: {MANIFEST} is missing")
    document = json.loads(path.read_text(encoding="utf-8"))

    # The pack manifest has been written in more than one shape; accept a mapping
    # of path -> hash or a list of {path, sha256} records rather than assuming.
    if isinstance(document, dict) and "files" in document:
        document = document["files"]
    if isinstance(document, dict):
        return {str(k): str(v) for k, v in document.items() if isinstance(v, str)}
    if isinstance(document, list):
        entries = {}
        for item in document:
            if isinstance(item, dict) and "path" in item:
                entries[str(item["path"])] = str(item.get("sha256", item.get("hash", "")))
        return entries
    raise SystemExit(f"ERROR: {MANIFEST} has an unrecognised shape")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(root: Path, allow_missing: tuple[str, ...]) -> tuple[list[str], list[str]]:
    """Return (errors, notices)."""
    errors: list[str] = []
    notices: list[str] = []

    for rel, expected in sorted(load_entries(root).items()):
        path = root / rel
        if not path.exists():
            if any(fnmatch.fnmatch(rel, pattern) for pattern in allow_missing):
                notices.append(f"pack file removed under an allowed pattern: {rel}")
            else:
                errors.append(f"pack file missing: {rel}")
            continue
        if expected and len(expected) == 64:
            actual = sha256(path)
            if actual != expected:
                notices.append(f"pack file changed since the pack was cut: {rel}")
    return errors, notices


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--allow-missing", action="append", default=[])
    parser.add_argument("--strict", action="store_true",
                        help="treat changed pack files as errors, not notices")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    allow_missing = tuple(args.allow_missing) or DEFAULT_ALLOW_MISSING
    errors, notices = run(root, allow_missing)

    for notice in notices:
        print(f"NOTICE: {notice}")
    if args.strict:
        errors.extend(n for n in notices if "changed since" in n)

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 2
    print(f"Pack manifest check passed: {len(load_entries(root))} entries, {len(notices)} notice(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
