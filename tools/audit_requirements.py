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
import re
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
STATUS_ENTRY_KEYS = set(EVIDENCE_KEYS) | {
    "status",
    "notes",
    "audit",
    "verification_blocked_until",
    "residual",
    "deferral_history",
}

# A verification obligation is a debt, and a debt that can be moved by editing one
# scalar is not a debt. Every obligation carries an append-only ledger; the head of
# the ledger must agree with the live field, so re-dating an obligation without
# recording that it was re-dated fails the audit.
DEFERRAL_ENTRY_KEYS = {"blocked_until", "recorded_at", "date", "reason"}
ISO_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

# Evidence directories are per-requirement by convention; a file sitting in one
# that no status entry cites is evidence nobody re-checks, which is how a stale
# artifact survives a milestone.
EVIDENCE_ROOT = "experiments/evidence"

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


def _check_deferral_history(rid: str, entry: dict[str, Any], blocked_until: Any, errors: list[str]) -> None:
    """The ledger behind ``verification_blocked_until``.

    Without this, moving an obligation from M1 to M4 is a one-character edit that
    no gate can distinguish from having always said M4. The history makes the move
    itself the thing that must be written down, and the head-agreement rule below
    means the live field cannot drift away from what was recorded.
    """
    history = entry.get("deferral_history")
    if history is None:
        errors.append(
            f"{rid}: verification_blocked_until requires a deferral_history "
            f"recording when and why the obligation was dated"
        )
        return
    if not isinstance(history, list) or not history:
        errors.append(f"{rid}: deferral_history must be a non-empty list")
        return

    previous_date = ""
    for index, item in enumerate(history):
        where = f"{rid}: deferral_history[{index}]"
        if not isinstance(item, dict):
            errors.append(f"{where} must be an object")
            continue
        missing = sorted(DEFERRAL_ENTRY_KEYS - set(item))
        if missing:
            errors.append(f"{where} is missing {', '.join(missing)}")
        unknown = sorted(set(item) - DEFERRAL_ENTRY_KEYS)
        if unknown:
            errors.append(f"{where} has unknown fields: {', '.join(unknown)}")
        for milestone_key in ("blocked_until", "recorded_at"):
            value = item.get(milestone_key)
            if value is not None and value not in VALID_MILESTONES:
                errors.append(f"{where}.{milestone_key} must be a milestone ID, got {value!r}")
        date = item.get("date")
        if not isinstance(date, str) or not ISO_DATE.match(date):
            errors.append(f"{where}.date must be an ISO date (YYYY-MM-DD), got {date!r}")
        elif date < previous_date:
            errors.append(f"{where}.date {date} is earlier than the entry before it ({previous_date})")
        else:
            previous_date = date
        reason = item.get("reason")
        if not isinstance(reason, str) or not reason.strip():
            errors.append(f"{where}.reason must say why the obligation was dated there")

    head = history[-1]
    if isinstance(head, dict) and head.get("blocked_until") != blocked_until:
        errors.append(
            f"{rid}: verification_blocked_until is {blocked_until!r} but the last "
            f"deferral_history entry records {head.get('blocked_until')!r} — an obligation "
            f"cannot be moved without recording the move"
        )


def _check_obligation(rid: str, entry: dict[str, Any], status: str, errors: list[str]) -> None:
    blocked_until = entry.get("verification_blocked_until")
    if blocked_until is None:
        if entry.get("residual"):
            errors.append(f"{rid}: residual recorded without verification_blocked_until")
        if entry.get("deferral_history"):
            errors.append(f"{rid}: deferral_history recorded without verification_blocked_until")
        return
    if blocked_until not in VALID_MILESTONES:
        errors.append(f"{rid}: verification_blocked_until must be a milestone ID, got {blocked_until!r}")
    if not entry.get("residual"):
        errors.append(f"{rid}: verification_blocked_until requires a residual describing what is missing")
    _check_deferral_history(rid, entry, blocked_until, errors)
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


def _check_unregistered_evidence(
    root: Path, statuses: dict[str, Any], errors: list[str]
) -> None:
    """Every artifact under ``experiments/evidence/<RID>/`` must be cited by <RID>.

    An artifact no status entry points at is re-checked by nothing: it can fall
    out of date with the tree and no gate notices, which is exactly how a stale
    test-count report survives a milestone. Citing it is what puts it under the
    evidence rules in :func:`_check_evidence`.
    """
    evidence_root = root / EVIDENCE_ROOT
    if not evidence_root.is_dir():
        return
    for directory in sorted(p for p in evidence_root.iterdir() if p.is_dir()):
        rid = directory.name
        entry = statuses.get(rid)
        cited: set[str] = set()
        if isinstance(entry, dict):
            for key in EVIDENCE_KEYS:
                value = entry.get(key, [])
                if isinstance(value, list):
                    cited.update(v for v in value if isinstance(v, str))
        for path in sorted(p for p in directory.rglob("*") if p.is_file()):
            rel = path.relative_to(root).as_posix()
            if rel not in cited:
                errors.append(
                    f"{rid}: evidence artifact is not registered in implementation_status.json: "
                    f"{rel} (register it or delete it — an uncited artifact is re-checked by nothing)"
                )


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
                history = entry.get("deferral_history")
                moves = len(history) - 1 if isinstance(history, list) and history else 0
                suffix = f"; re-dated {moves} time(s) since first registered" if moves else ""
                errors.append(
                    f"{rid}: verification obligation due at {check_deferred} is still open "
                    f"({entry.get('residual', 'no residual recorded')}){suffix}"
                )

    if deep:
        _check_unregistered_evidence(root, statuses, errors)
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
