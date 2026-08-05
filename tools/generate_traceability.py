#!/usr/bin/env python3
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
reqs = json.loads((ROOT/"requirements/requirements.json").read_text())["requirements"]
status = json.loads((ROOT/"requirements/implementation_status.json").read_text())["requirements"]
lines = ["# Feature Traceability Matrix", "",
         "| ID | Module | Milestone | Requirement | Status | Evidence |",
         "|---|---|---:|---|---|---|"]
for r in reqs:
    s = status[r["id"]]
    evidence = s["implementation"] + s["tests"] + s["reports"]
    lines.append(f"| {r['id']} | {r['module']} | {r['milestone']} | {r['title']} | {s['status']} | {'<br>'.join(evidence) if evidence else '—'} |")
(ROOT/"docs/TRACEABILITY_MATRIX.md").write_text("\n".join(lines)+"\n", encoding="utf-8")
print(f"Generated traceability matrix with {len(reqs)} requirements")
