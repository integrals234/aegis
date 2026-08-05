#!/usr/bin/env python3
import json, os, sys
from pathlib import Path

FROZEN = {
    "docs/MASTER_SPEC.md",
    "requirements/requirements.json",
    "docs/BENCHMARK_POLICY.md",
    "docs/CV_CLAIMS_POLICY.md",
    "docs/DATA_AND_RESEARCH_POLICY.md",
}

if os.environ.get("AEGIS_ALLOW_SPEC_EDIT") == "1":
    sys.exit(0)

try:
    payload = json.load(sys.stdin)
except Exception:
    sys.exit(0)

tool_input = payload.get("tool_input", {}) or {}
candidates = []
for key in ("file_path", "path", "filename"):
    value = tool_input.get(key)
    if isinstance(value, str):
        candidates.append(value)
for key in ("files", "edits"):
    value = tool_input.get(key)
    if isinstance(value, list):
        for item in value:
            if isinstance(item, dict):
                for k in ("file_path", "path", "filename"):
                    if isinstance(item.get(k), str):
                        candidates.append(item[k])

project = Path(os.environ.get("CLAUDE_PROJECT_DIR", ".")).resolve()
for raw in candidates:
    p = Path(raw)
    try:
        rel = (p if p.is_absolute() else project / p).resolve().relative_to(project).as_posix()
    except Exception:
        continue
    if rel in FROZEN:
        print(
            f"Blocked: {rel} is a frozen AEGIS specification file. "
            "The owner must edit it manually and intentionally version the specification.",
            file=sys.stderr,
        )
        sys.exit(2)
sys.exit(0)
