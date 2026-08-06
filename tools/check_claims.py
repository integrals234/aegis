#!/usr/bin/env python3
"""Reject unsupported performance, trading and CV claims (AEGIS-006).

Two rules, both from ``docs/CV_CLAIMS_POLICY.md``:

1. Forbidden phrasing ("guaranteed profit", "institutional-grade", …) never
   appears as an AEGIS claim.
2. A sentence that puts a number next to a performance or trading unit must
   carry an ``evidence:`` marker naming a path that exists. A latency figure
   with no artifact behind it is exactly the failure this requirement exists to
   prevent, and prose cannot be audited later.

Exclusions live in ``configs/claims_policy.yaml`` with a reason each. The
alternative — loosening the patterns until the policy documents stop matching —
would disable the control while leaving it looking enabled.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_POLICY = "configs/claims_policy.yaml"

SENTENCE_SPLIT = re.compile(r"(?<=[.!?])\s+|\n")
NUMBER = re.compile(r"\b\d+(?:[.,]\d+)*\b")
CODE_FENCE = re.compile(r"```.*?```", re.DOTALL)
INLINE_CODE = re.compile(r"`[^`]*`")


@dataclass(frozen=True)
class Claim:
    path: str
    line: int
    rule: str
    text: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}: {self.text}"


def load_policy(path: Path) -> dict[str, Any]:
    doc: dict[str, Any] = yaml.safe_load(path.read_text(encoding="utf-8"))
    for entry in doc.get("excluded_paths", []):
        if not entry.get("reason"):
            raise SystemExit(f"ERROR: excluded path {entry.get('path')!r} has no reason")
    return doc


def fully_excluded(policy: dict[str, Any]) -> set[str]:
    """Paths skipped entirely; entries naming `phrases` are only partly exempt."""
    return {e["path"] for e in policy.get("excluded_paths", []) if not e.get("phrases")}


def exempt_phrases(policy: dict[str, Any], rel: str) -> set[str]:
    exempt: set[str] = set()
    for entry in policy.get("excluded_paths", []):
        if entry["path"] == rel:
            exempt.update(phrase.lower() for phrase in entry.get("phrases", []))
    return exempt


def scanned_files(root: Path, policy: dict[str, Any]) -> list[Path]:
    excluded = fully_excluded(policy)
    files: set[Path] = set()
    for pattern in policy["scanned_globs"]:
        files.update(p for p in root.glob(pattern) if p.is_file())
    return sorted(
        p for p in files if p.relative_to(root).as_posix() not in excluded and ".venv" not in p.parts
    )


def strip_code(text: str) -> str:
    """Commands and sample output are not claims; prose is."""
    text = CODE_FENCE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return INLINE_CODE.sub("", text)


def evidence_paths_resolve(root: Path, sentence: str, marker: str) -> bool:
    """An evidence marker only counts when the path it names exists."""
    found = False
    for raw in re.findall(rf"{re.escape(marker)}\s*([^\s,;)]+)", sentence, flags=re.IGNORECASE):
        found = True
        if not (root / raw.strip("`'\"")).exists():
            return False
    return found


def check_file(root: Path, path: Path, policy: dict[str, Any]) -> list[Claim]:
    rel = path.relative_to(root).as_posix()
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = strip_code(raw)
    claims: list[Claim] = []

    exempt = exempt_phrases(policy, rel)
    lowered_phrases = [phrase.lower() for phrase in policy["forbidden_phrases"]]
    for lineno, line in enumerate(text.splitlines(), start=1):
        lowered = line.lower()
        for phrase, original in zip(lowered_phrases, policy["forbidden_phrases"], strict=True):
            if phrase in exempt:
                continue
            if phrase in lowered:
                claims.append(Claim(rel, lineno, "forbidden phrasing", f"{original!r} in: {line.strip()}"))

    units = policy["claim_units"]
    unit_pattern = re.compile(
        r"(?:\b\d+(?:[.,]\d+)*\s*(?:" + "|".join(re.escape(u) for u in units) + r")\b)"
        r"|(?:\b(?:" + "|".join(re.escape(u) for u in units) + r")\b[^.\n]{0,20}?\b\d+(?:[.,]\d+)*\b)",
        re.IGNORECASE,
    )
    marker = policy["evidence_marker"]

    offset = 0
    for chunk in SENTENCE_SPLIT.split(text):
        line_no = text.count("\n", 0, offset) + 1
        offset += len(chunk) + 1
        sentence = chunk.strip()
        if not sentence or not NUMBER.search(sentence):
            continue
        if not unit_pattern.search(sentence):
            continue
        if evidence_paths_resolve(root, sentence, marker):
            continue
        claims.append(
            Claim(
                rel,
                line_no,
                "numeric claim without resolvable evidence",
                sentence[:160],
            )
        )
    return claims


def run(root: Path, policy_path: Path) -> list[Claim]:
    policy = load_policy(policy_path)
    claims: list[Claim] = []
    for path in scanned_files(root, policy):
        claims.extend(check_file(root, path, policy))
    return claims


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--policy", type=Path)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    claims = run(root, args.policy or root / DEFAULT_POLICY)
    if claims:
        for claim in claims:
            print(f"ERROR: {claim}", file=sys.stderr)
        print(f"Claims audit failed: {len(claims)} unsupported claim(s)", file=sys.stderr)
        return 2
    print("Claims audit passed: no forbidden phrasing and no unevidenced numeric claims")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
