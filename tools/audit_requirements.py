#!/usr/bin/env python3
import argparse, json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REQ_PATH = ROOT / "requirements/requirements.json"
STATUS_PATH = ROOT / "requirements/implementation_status.json"
REQUIRED_MODULES = {
    "Governance", "Futures Data & Contract Lifecycle", "Deterministic LOB & Matching",
    "Low-Latency Engineering", "Historical Replay", "Market Data & Book Reconstruction",
    "Quantitative Research", "Online Statistics", "OMS & Execution", "Risk & Portfolio",
    "Validation & Anti-Overfitting", "Performance & Execution Attribution",
    "Trader Decision Arena", "Counterfactual Decision Intelligence",
    "Confidence & Behaviour Analytics", "Dashboard & Experiment Management",
    "Paper Trading Path", "Engineering Platform"
}
VALID = {"not_started", "in_progress", "blocked", "implemented", "verified", "deferred"}

def fail(errors):
    for e in errors:
        print(f"ERROR: {e}", file=sys.stderr)
    return 2 if errors else 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--milestone")
    args = ap.parse_args()

    errors = []
    req_doc = json.loads(REQ_PATH.read_text(encoding="utf-8"))
    status_doc = json.loads(STATUS_PATH.read_text(encoding="utf-8"))
    reqs = req_doc["requirements"]
    statuses = status_doc["requirements"]

    ids = [r["id"] for r in reqs]
    if len(ids) != len(set(ids)):
        errors.append("duplicate requirement IDs")
    req_ids = set(ids)
    status_ids = set(statuses)
    for missing in sorted(req_ids - status_ids):
        errors.append(f"missing status entry: {missing}")
    for orphan in sorted(status_ids - req_ids):
        errors.append(f"orphan status entry: {orphan}")
    modules = {r["module"] for r in reqs}
    for m in sorted(REQUIRED_MODULES - modules):
        errors.append(f"required module absent: {m}")

    selected = [r for r in reqs if not args.milestone or r["milestone"] == args.milestone]
    for r in selected:
        st = statuses.get(r["id"], {})
        value = st.get("status")
        if value not in VALID:
            errors.append(f"{r['id']}: invalid status {value!r}")
            continue
        evidence = []
        for key in ("implementation", "tests", "reports"):
            vals = st.get(key, [])
            if not isinstance(vals, list):
                errors.append(f"{r['id']}: {key} must be a list")
                continue
            evidence.extend(vals)
            for rel in vals:
                p = ROOT / rel
                if not p.exists():
                    errors.append(f"{r['id']}: evidence path does not exist: {rel}")
        if value == "verified":
            if not st.get("implementation"):
                errors.append(f"{r['id']}: verified without implementation evidence")
            if not (st.get("tests") or st.get("reports")):
                errors.append(f"{r['id']}: verified without test/report evidence")
        if value == "implemented" and not st.get("implementation"):
            errors.append(f"{r['id']}: implemented without implementation path")
        if value == "deferred" and r.get("priority") == "must":
            errors.append(f"{r['id']}: MUST requirement cannot be silently deferred")

    # Frozen hash check
    manifest_path = ROOT / "requirements/frozen_hashes.json"
    if manifest_path.exists():
        import hashlib
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for rel, expected in manifest.items():
            p = ROOT / rel
            if not p.exists():
                errors.append(f"frozen file missing: {rel}")
            else:
                actual = hashlib.sha256(p.read_bytes()).hexdigest()
                if actual != expected:
                    errors.append(f"frozen file changed without version update: {rel}")

    if errors:
        return fail(errors)

    counts = {}
    for r in selected:
        s = statuses[r["id"]]["status"]
        counts[s] = counts.get(s, 0) + 1
    print(f"AEGIS requirements audit passed: {len(selected)} requirements checked")
    print("Status:", ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
