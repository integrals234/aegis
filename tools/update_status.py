#!/usr/bin/env python3
import argparse, json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
P = ROOT / "requirements/implementation_status.json"

ap = argparse.ArgumentParser()
ap.add_argument("requirement_id")
ap.add_argument("status", choices=["not_started","in_progress","blocked","implemented","verified","deferred"])
ap.add_argument("--implementation", action="append", default=[])
ap.add_argument("--test", action="append", default=[])
ap.add_argument("--report", action="append", default=[])
ap.add_argument("--note", default="")
args = ap.parse_args()

doc = json.loads(P.read_text(encoding="utf-8"))
if args.requirement_id not in doc["requirements"]:
    raise SystemExit(f"Unknown requirement: {args.requirement_id}")
entry = doc["requirements"][args.requirement_id]
entry["status"] = args.status
for key, values in [("implementation",args.implementation),("tests",args.test),("reports",args.report)]:
    for v in values:
        if v not in entry[key]:
            entry[key].append(v)
if args.note:
    entry["notes"] = args.note
P.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
print(f"Updated {args.requirement_id} -> {args.status}")
