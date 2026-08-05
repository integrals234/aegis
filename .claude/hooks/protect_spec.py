#!/usr/bin/env python3
"""PreToolUse hook: refuse Claude-tool writes to frozen AEGIS files (AEGIS-001).

This hook is *defence in depth*, not the control of record. It only observes
writes made through Claude's file tools; a shell redirect never reaches it. The
authoritative controls are ``tools/check_frozen.py`` (hash + branch diff),
``tools/audit_requirements.py`` (hash), the pre-commit hook and CI.

Two properties matter here:

* **Fail closed.** A hook that exits 0 on malformed input is a hook an
  unexpected payload shape switches off. Anything this script cannot parse or
  resolve is treated as a block, not as permission.
* **Protect the manifest too.** Guarding the specification while leaving
  ``requirements/frozen_hashes.json`` writable protects the document from
  editing but not from having its tamper detector rewritten.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

PROTECTED = {
    "docs/MASTER_SPEC.md",
    "requirements/requirements.json",
    "docs/BENCHMARK_POLICY.md",
    "docs/CV_CLAIMS_POLICY.md",
    "docs/DATA_AND_RESEARCH_POLICY.md",
    # The tamper detector for everything above.
    "requirements/frozen_hashes.json",
}

PATH_KEYS = ("file_path", "path", "filename", "notebook_path")
NESTED_KEYS = ("files", "edits", "operations")

BLOCK = 2
ALLOW = 0


def deny(message: str) -> None:
    print(f"Blocked: {message}", file=sys.stderr)
    raise SystemExit(BLOCK)


def collect_paths(tool_input: object) -> list[str]:
    """Pull every candidate path out of a tool payload of unknown shape."""
    found: list[str] = []
    if isinstance(tool_input, dict):
        for key, value in tool_input.items():
            if key in PATH_KEYS and isinstance(value, str):
                found.append(value)
            elif key in NESTED_KEYS or isinstance(value, (dict, list)):
                found.extend(collect_paths(value))
    elif isinstance(tool_input, list):
        for item in tool_input:
            found.extend(collect_paths(item))
    return found


def main() -> int:
    # The owner's documented escape hatch for intentional specification edits.
    if os.environ.get("AEGIS_ALLOW_SPEC_EDIT") == "1":
        return ALLOW

    raw = sys.stdin.read()
    if not raw.strip():
        deny("hook received no payload; refusing the write rather than assuming it is safe.")
    try:
        payload = json.loads(raw)
    except (ValueError, TypeError):
        deny("hook could not parse the tool payload; refusing the write rather than failing open.")
    if not isinstance(payload, dict):
        deny("hook received a non-object payload; refusing the write rather than failing open.")

    project_dir = os.environ.get("CLAUDE_PROJECT_DIR")
    if not project_dir:
        deny("CLAUDE_PROJECT_DIR is unset, so frozen paths cannot be resolved; refusing the write.")
    project = Path(project_dir).resolve()

    for raw_path in collect_paths(payload.get("tool_input")):
        candidate = Path(raw_path)
        absolute = candidate if candidate.is_absolute() else project / candidate
        try:
            rel = absolute.resolve().relative_to(project).as_posix()
        except (ValueError, OSError):
            continue  # outside the project; not this hook's business
        if rel in PROTECTED:
            deny(
                f"{rel} is a frozen AEGIS governance file. The owner must edit it manually and "
                "version the specification intentionally. See docs/BUILD_STATE.md."
            )
    return ALLOW


if __name__ == "__main__":
    raise SystemExit(main())
