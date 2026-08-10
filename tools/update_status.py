#!/usr/bin/env python3
"""Update requirements/implementation_status.json under the AEGIS evidence rules.

The tracker is the only place a completion claim is recorded, so this tool
refuses to write a claim the auditor would reject: no ``verified`` without an
independent audit record, no evidence path that does not exist, and no
``verified`` while a verification obligation is registered against the entry.
Writing the claim and checking the claim share predicates with
:mod:`audit_requirements`, so the two cannot drift apart.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from audit_requirements import (
    EVIDENCE_KEYS,
    VALID_MILESTONES,
    VALID_STATUS,
    _is_substantive,
    load_json,
)

ROOT = Path(__file__).resolve().parents[1]
STATUS_PATH = ROOT / "requirements/implementation_status.json"


def _validate(rid: str, entry: dict[str, Any], root: Path) -> list[str]:
    errors: list[str] = []
    status = entry["status"]

    for key in EVIDENCE_KEYS:
        for rel in entry.get(key, []):
            if not (root / rel).exists():
                errors.append(f"{rid}: {key} path does not exist: {rel}")

    if status == "implemented" and not entry.get("implementation"):
        errors.append(f"{rid}: 'implemented' requires at least one --implementation path")

    if status == "verified":
        if entry.get("verification_blocked_until"):
            errors.append(
                f"{rid}: cannot be 'verified' while blocked until "
                f"{entry['verification_blocked_until']}; resolve the obligation first"
            )
        if not entry.get("implementation"):
            errors.append(f"{rid}: 'verified' requires implementation evidence")
        proof = list(entry.get("tests", [])) + list(entry.get("reports", []))
        if not proof:
            errors.append(f"{rid}: 'verified' requires test or report evidence")
        elif not any(_is_substantive(root / rel)[0] for rel in proof):
            errors.append(f"{rid}: 'verified' requires a non-empty, non-placeholder test/report artifact")
        audit = entry.get("audit")
        if not isinstance(audit, dict) or not all(audit.get(k) for k in ("auditor", "commit", "date")):
            errors.append(
                f"{rid}: 'verified' requires --auditor, --audit-commit and --audit-date "
                "recording the independent review"
            )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("requirement_id")
    parser.add_argument("status", choices=sorted(VALID_STATUS))
    parser.add_argument("--implementation", action="append", default=[])
    parser.add_argument("--test", action="append", default=[])
    parser.add_argument("--report", action="append", default=[])
    parser.add_argument("--note", default="")
    parser.add_argument("--auditor", help="independent reviewer that verified the requirement")
    parser.add_argument("--audit-commit", help="commit the independent review covered")
    parser.add_argument("--audit-date", help="ISO date of the independent review")
    parser.add_argument(
        "--blocked-until", choices=sorted(VALID_MILESTONES),
        help="milestone that unblocks verification",
    )
    parser.add_argument("--residual", help="what is still missing; required with --blocked-until")
    parser.add_argument(
        "--clear-obligation", action="store_true",
        help="remove a registered verification obligation",
    )
    parser.add_argument("--status-path", type=Path, default=STATUS_PATH)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    doc = load_json(args.status_path)
    if args.requirement_id not in doc["requirements"]:
        print(f"ERROR: unknown requirement: {args.requirement_id}", file=sys.stderr)
        return 2

    entry = doc["requirements"][args.requirement_id]
    entry["status"] = args.status
    for key, values in (("implementation", args.implementation), ("tests", args.test), ("reports", args.report)):
        for value in values:
            if value not in entry[key]:
                entry[key].append(value)
    if args.note:
        entry["notes"] = args.note
    if args.auditor or args.audit_commit or args.audit_date:
        entry["audit"] = {
            "auditor": args.auditor or "",
            "commit": args.audit_commit or "",
            "date": args.audit_date or "",
        }
    if args.clear_obligation:
        entry.pop("verification_blocked_until", None)
        entry.pop("residual", None)
        # audit_requirements.py requires deferral_history and
        # verification_blocked_until to travel together (a ledger with no
        # live obligation to explain is a dangling record); a genuinely
        # discharged obligation drops both, matching AEGIS-227/233/234/009's
        # precedent from the M0->M1 discharges.
        entry.pop("deferral_history", None)
    if args.blocked_until:
        if not args.residual:
            print("ERROR: --blocked-until requires --residual", file=sys.stderr)
            return 2
        entry["verification_blocked_until"] = args.blocked_until
        entry["residual"] = args.residual

    errors = _validate(args.requirement_id, entry, root)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print("Refusing to record an unsupportable status claim.", file=sys.stderr)
        return 2

    args.status_path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"Updated {args.requirement_id} -> {args.status}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
