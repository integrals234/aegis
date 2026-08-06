#!/usr/bin/env python3
"""Audit the AEGIS requirement catalogue, status tracker and evidence.

This is the control of record for AEGIS-002 (traceability) and AEGIS-003
(evidence-based completion). Everything it checks is checkable: it never trusts
prose in ``notes``, and it treats an evidence path that exists but says nothing
(an empty file, a ``.gitkeep``, a directory, a file containing only TODOs) as
the absence of evidence.

The module exposes :func:`run_audit` so the auditor itself can be tested against
fixture trees instead of only against the live repository.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]

REQUIRED_MODULES = {
    "Governance",
    "Futures Data & Contract Lifecycle",
    "Deterministic LOB & Matching",
    "Low-Latency Engineering",
    "Historical Replay",
    "Market Data & Book Reconstruction",
    "Quantitative Research",
    "Online Statistics",
    "OMS & Execution",
    "Risk & Portfolio",
    "Validation & Anti-Overfitting",
    "Performance & Execution Attribution",
    "Trader Decision Arena",
    "Counterfactual Decision Intelligence",
    "Confidence & Behaviour Analytics",
    "Dashboard & Experiment Management",
    "Paper Trading Path",
    "Engineering Platform",
}
VALID_STATUS = {"not_started", "in_progress", "blocked", "implemented", "verified", "deferred"}
VALID_MILESTONES = {f"M{n}" for n in range(10)}
EVIDENCE_KEYS = ("implementation", "tests", "reports")
STATUS_ENTRY_KEYS = set(EVIDENCE_KEYS) | {"status", "notes", "audit", "verification_blocked_until", "residual"}

# Paths that exist but carry no information about whether anything works.
NON_EVIDENCE_NAMES = {".gitkeep", ".gitignore", "__init__.py", "py.typed"}
PLACEHOLDER_MARKERS = ("TODO", "TBD", "FIXME", "PLACEHOLDER", "XXX")


class DuplicateKeyError(ValueError):
    """Raised when a JSON object repeats a key.

    ``json.loads`` silently keeps the last occurrence, so a requirement ID
    pasted twice with different evidence would audit clean while the catalogue
    is quietly self-contradictory.
    """


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, Any]:
    seen: set[str] = set()
    for key, _ in pairs:
        if key in seen:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        seen.add(key)
    return dict(pairs)


def load_json(path: Path) -> dict[str, Any]:
    loaded: dict[str, Any] = json.loads(
        path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_keys
    )
    return loaded


@dataclass
class AuditResult:
    errors: list[str] = field(default_factory=list)
    counts: dict[str, int] = field(default_factory=dict)
    checked: int = 0

    @property
    def ok(self) -> bool:
        return not self.errors


def _is_substantive(path: Path) -> tuple[bool, str]:
    """Decide whether a path is evidence or merely a path that exists."""
    if not path.exists():
        return False, "does not exist"
    if path.is_dir():
        return False, "is a directory"
    if path.name in NON_EVIDENCE_NAMES:
        return False, f"is a {path.name} placeholder file"
    try:
        size = path.stat().st_size
    except OSError as exc:  # pragma: no cover - unreadable file on a normal checkout
        return False, f"cannot be read: {exc}"
    if size == 0:
        return False, "is empty"
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return True, ""  # binary artifact (e.g. a benchmark capture) is real content
    meaningful = [
        line.strip()
        for line in text.splitlines()
        if line.strip() and not line.strip().startswith(("#", "//"))
    ]
    if not meaningful:
        return False, "contains no content outside comments"
    if all(any(marker in line.upper() for marker in PLACEHOLDER_MARKERS) for line in meaningful):
        return False, "contains only TODO/placeholder text"
    return True, ""


def _check_entry_shape(rid: str, entry: object, errors: list[str]) -> dict[str, Any] | None:
    if not isinstance(entry, dict):
        errors.append(f"{rid}: status entry must be an object")
        return None
    unknown = sorted(set(entry) - STATUS_ENTRY_KEYS)
    if unknown:
        errors.append(f"{rid}: unknown status fields: {', '.join(unknown)}")
    for key in EVIDENCE_KEYS:
        value = entry.get(key, [])
        if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
            errors.append(f"{rid}: {key} must be a list of strings")
    return entry


def _check_obligation(rid: str, entry: dict[str, Any], status: str, errors: list[str]) -> None:
    blocked_until = entry.get("verification_blocked_until")
    if blocked_until is None:
        if entry.get("residual"):
            errors.append(f"{rid}: residual recorded without verification_blocked_until")
        return
    if blocked_until not in VALID_MILESTONES:
        errors.append(f"{rid}: verification_blocked_until must be a milestone ID, got {blocked_until!r}")
    if not entry.get("residual"):
        errors.append(f"{rid}: verification_blocked_until requires a residual describing what is missing")
    if status == "verified":
        # The anti-inflation rule as a gate rather than a convention.
        errors.append(
            f"{rid}: cannot be 'verified' while verification is blocked until {blocked_until}"
        )


def _check_evidence(
    rid: str, entry: dict[str, Any], status: str, root: Path, deep: bool, errors: list[str]
) -> None:
    for key in EVIDENCE_KEYS:
        for rel in entry.get(key, []):
            if Path(rel).is_absolute() or ".." in Path(rel).parts:
                errors.append(f"{rid}: evidence path must be repository-relative: {rel}")
                continue
            if not (root / rel).exists():
                errors.append(f"{rid}: evidence path does not exist: {rel}")

    if status == "implemented":
        if not entry.get("implementation"):
            errors.append(f"{rid}: implemented without implementation path")
        elif deep:
            substantive = [
                rel for rel in entry["implementation"] if _is_substantive(root / rel)[0]
            ]
            if not substantive:
                errors.append(
                    f"{rid}: implemented but no implementation path is a non-empty, non-placeholder file"
                )

    if status != "verified":
        return

    if not entry.get("implementation"):
        errors.append(f"{rid}: verified without implementation evidence")
    proof = list(entry.get("tests", [])) + list(entry.get("reports", []))
    if not proof:
        errors.append(f"{rid}: verified without test/report evidence")
    elif deep:
        accepted = False
        for rel in proof:
            good, why = _is_substantive(root / rel)
            if good:
                accepted = True
            else:
                errors.append(f"{rid}: evidence {rel} {why}")
        if not accepted:
            errors.append(f"{rid}: verified without a single substantive test/report artifact")

    audit = entry.get("audit")
    if not isinstance(audit, dict):
        errors.append(f"{rid}: verified requires an 'audit' object (auditor, commit, date)")
    else:
        for required in ("auditor", "commit", "date"):
            if not audit.get(required):
                errors.append(f"{rid}: audit.{required} is required for 'verified'")


def _check_frozen(root: Path, manifest_path: Path, errors: list[str]) -> None:
    if not manifest_path.exists():
        errors.append(f"frozen manifest missing: {manifest_path}")
        return
    manifest = load_json(manifest_path)
    for rel, expected in manifest.items():
        path = root / rel
        if not path.exists():
            errors.append(f"frozen file missing: {rel}")
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            errors.append(f"frozen file changed without version update: {rel}")


def run_audit(
    req_path: Path,
    status_path: Path,
    root: Path,
    milestone: str | None = None,
    deep: bool = True,
    frozen_manifest: Path | None = None,
    check_deferred: str | None = None,
) -> AuditResult:
    """Audit a catalogue/status pair rooted at ``root``.

    ``deep`` enables evidence-content inspection; ``--quick`` turns it off so the
    structural checks stay usable as a fast editor hook.
    """
    result = AuditResult()
    errors = result.errors

    try:
        req_doc = load_json(req_path)
        status_doc = load_json(status_path)
    except DuplicateKeyError as exc:
        errors.append(str(exc))
        return result
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"cannot load requirement documents: {exc}")
        return result

    reqs = req_doc.get("requirements")
    statuses = status_doc.get("requirements")
    if not isinstance(reqs, list) or not isinstance(statuses, dict):
        errors.append("malformed documents: requirements must be a list and statuses an object")
        return result

    ids = [r["id"] for r in reqs]
    if len(ids) != len(set(ids)):
        duplicates = sorted({i for i in ids if ids.count(i) > 1})
        errors.append(f"duplicate requirement IDs: {', '.join(duplicates)}")
    req_ids = set(ids)
    for missing in sorted(req_ids - set(statuses)):
        errors.append(f"missing status entry: {missing}")
    for orphan in sorted(set(statuses) - req_ids):
        errors.append(f"orphan status entry: {orphan}")
    for module in sorted(REQUIRED_MODULES - {r["module"] for r in reqs}):
        errors.append(f"required module absent: {module}")
    for r in reqs:
        if r.get("milestone") not in VALID_MILESTONES:
            errors.append(f"{r['id']}: unknown milestone {r.get('milestone')!r}")

    selected = [r for r in reqs if milestone is None or r.get("milestone") == milestone]
    if milestone is not None and not selected:
        errors.append(f"no requirements found for milestone {milestone}")
    result.checked = len(selected)

    for r in selected:
        rid = r["id"]
        raw = statuses.get(rid)
        if raw is None:
            continue  # already reported as a missing status entry
        entry = _check_entry_shape(rid, raw, errors)
        if entry is None:
            continue
        status = entry.get("status")
        if status not in VALID_STATUS:
            errors.append(f"{rid}: invalid status {status!r}")
            continue
        if status == "deferred" and r.get("priority") == "must":
            errors.append(f"{rid}: MUST requirement cannot be silently deferred")
        _check_obligation(rid, entry, status, errors)
        _check_evidence(rid, entry, status, root, deep, errors)
        result.counts[status] = result.counts.get(status, 0) + 1

    if check_deferred:
        for rid, entry in sorted(statuses.items()):
            if not isinstance(entry, dict):
                continue
            if entry.get("verification_blocked_until") == check_deferred:
                errors.append(
                    f"{rid}: verification obligation due at {check_deferred} is still open "
                    f"({entry.get('residual', 'no residual recorded')})"
                )

    _check_frozen(root, frozen_manifest or (root / "requirements/frozen_hashes.json"), errors)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--quick", action="store_true", help="structural checks only; skip evidence-content inspection"
    )
    parser.add_argument("--milestone", help="restrict per-requirement checks to one milestone")
    parser.add_argument(
        "--check-deferred", metavar="MILESTONE",
        help="fail if any obligation due at MILESTONE is still open",
    )
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--requirements", type=Path)
    parser.add_argument("--status", type=Path)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    result = run_audit(
        req_path=args.requirements or root / "requirements/requirements.json",
        status_path=args.status or root / "requirements/implementation_status.json",
        root=root,
        milestone=args.milestone,
        deep=not args.quick,
        check_deferred=args.check_deferred,
    )
    if not result.ok:
        for error in result.errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 2
    scope = args.milestone or "all milestones"
    print(f"AEGIS requirements audit passed: {result.checked} requirements checked ({scope})")
    print("Status:", ", ".join(f"{k}={v}" for k, v in sorted(result.counts.items())) or "none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
