#!/usr/bin/env python3
"""Scan the worktree, the index and git history for committed secrets.

AEGIS-010. Three surfaces matter and they fail differently:

* **worktree** — what a reviewer sees;
* **index** — what the next commit will publish, which the worktree scan misses
  when a file is staged and then reverted on disk;
* **history** — a key removed in a later commit is still in the repository, and
  a worktree-only scanner reports it clean.

The tool uses only the standard library so it can run in a pre-commit hook
before any environment is installed.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST = "configs/secret_scan_allowlist.json"

# Patterns are written so their own source text cannot match them; the scanner
# scanning itself must not be a finding.
RULES: tuple[tuple[str, re.Pattern[str], str], ...] = (
    (
        "private-key-block",
        re.compile(r"-----BEGIN (?:[A-Z]+ )?PRIVATE KEY-----"),
        "PEM private key material",
    ),
    (
        "aws-access-key-id",
        re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
        "AWS access key ID",
    ),
    (
        "github-token",
        re.compile(r"\bgh[pousr]_[A-Za-z0-9]{36,}\b"),
        "GitHub personal access token",
    ),
    (
        "slack-token",
        re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{12,}\b"),
        "Slack token",
    ),
    (
        "json-web-token",
        re.compile(r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b"),
        "JSON Web Token",
    ),
    (
        "assigned-credential",
        re.compile(
            r"""(?ix)
            \b (?: api[_-]?key | secret[_-]?key | client[_-]?secret
                 | password | passwd | auth[_-]?token | access[_-]?token )
            \b \s* [:=] \s* ["'] [^"'\s]{8,} ["']
            """
        ),
        "credential assigned to a literal string",
    ),
)

# Paths that must never be tracked at all, regardless of content.
FORBIDDEN_PATHS = (
    ".env",
    "*/.env",
    ".env.*",
    "*/.env.*",
    "secrets/*",
    "*/secrets/*",
    "credentials/*",
    "*/credentials/*",
    "private/*",
    "*/private/*",
    "*.pem",
    "*.key",
    "id_rsa",
    "id_rsa.*",
)
FORBIDDEN_EXCEPTIONS = ("*.env.example", "*.env.template")

BINARY_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".pdf", ".parquet", ".arrow", ".so", ".o", ".whl"}


@dataclass(frozen=True)
class Finding:
    surface: str
    location: str
    rule: str
    detail: str

    def __str__(self) -> str:
        return f"[{self.surface}] {self.location}: {self.rule} ({self.detail})"


def load_allowlist(root: Path) -> list[dict[str, str]]:
    path = root / ALLOWLIST
    if not path.exists():
        return []
    entries = json.loads(path.read_text(encoding="utf-8"))["allow"]
    for entry in entries:
        if not entry.get("reason"):
            raise SystemExit(f"ERROR: {ALLOWLIST} entry {entry.get('path')!r} has no reason")
    return entries


def is_allowlisted(path: str, allowlist: list[dict[str, str]]) -> bool:
    return any(fnmatch.fnmatch(path, entry["path"]) for entry in allowlist)


def git(root: Path, *args: str) -> str:
    result = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"ERROR: git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def scan_text(surface: str, location: str, text: str) -> list[Finding]:
    findings = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        for rule, pattern, detail in RULES:
            if pattern.search(line):
                findings.append(Finding(surface, f"{location}:{lineno}", rule, detail))
    return findings


def check_forbidden_path(path: str) -> str | None:
    if any(fnmatch.fnmatch(path, pattern) for pattern in FORBIDDEN_EXCEPTIONS):
        return None
    for pattern in FORBIDDEN_PATHS:
        if fnmatch.fnmatch(path, pattern):
            return pattern
    return None


def scan_worktree(root: Path, allowlist: list[dict[str, str]]) -> list[Finding]:
    findings: list[Finding] = []
    for path in git(root, "ls-files").splitlines():
        if not path or is_allowlisted(path, allowlist):
            continue
        pattern = check_forbidden_path(path)
        if pattern:
            findings.append(Finding("worktree", path, "tracked-sensitive-path", f"matches {pattern}"))
        file_path = root / path
        if file_path.suffix in BINARY_SUFFIXES or not file_path.is_file():
            continue
        try:
            text = file_path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        findings.extend(scan_text("worktree", path, text))
    return findings


def scan_staged(root: Path, allowlist: list[dict[str, str]]) -> list[Finding]:
    findings: list[Finding] = []
    staged = [p for p in git(root, "diff", "--cached", "--name-only", "--diff-filter=ACMR").splitlines() if p]
    for path in staged:
        # The allowlist is applied per file, so a fixture that is allowed to
        # contain secret-shaped text does not have to be allowed again here.
        if is_allowlisted(path, allowlist):
            continue
        pattern = check_forbidden_path(path)
        if pattern:
            findings.append(Finding("index", path, "staged-sensitive-path", f"matches {pattern}"))
        if Path(path).suffix in BINARY_SUFFIXES:
            continue
        # Read the staged blob, not the worktree file: a secret can be staged and
        # then reverted on disk.
        try:
            content = git(root, "show", f":{path}")
        except SystemExit:
            continue
        findings.extend(scan_text("index", path, content))
    return findings


def scan_history(root: Path, limit: int | None, allowlist: list[dict[str, str]]) -> list[Finding]:
    args = ["log", "--all", "-p", "--no-color", "--format=commit %H"]
    if limit:
        args.insert(1, f"-n{limit}")
    findings: list[Finding] = []
    commit = "unknown"
    path = "unknown"
    for line in git(root, *args).splitlines():
        if line.startswith("commit "):
            commit = line.split(" ", 1)[1][:12]
            continue
        if line.startswith("+++ "):
            # Track which file the following added lines belong to, so the same
            # per-path allowlist applies here as in the worktree scan.
            target = line[4:].strip()
            path = target[2:] if target.startswith("b/") else target
            continue
        if not line.startswith("+"):
            continue
        if is_allowlisted(path, allowlist):
            continue
        for rule, pattern, detail in RULES:
            if pattern.search(line[1:]):
                findings.append(Finding("history", f"{path} (commit {commit})", rule, detail))
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--staged", action="store_true", help="also scan the git index")
    parser.add_argument("--history", action="store_true", help="also scan every commit reachable from any ref")
    parser.add_argument("--history-limit", type=int, default=None)
    parser.add_argument("--path", type=Path, help="scan a single file or directory instead of the repository")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    allowlist = load_allowlist(root)
    findings: list[Finding] = []

    if args.path:
        target = args.path.resolve()
        files = [target] if target.is_file() else sorted(p for p in target.rglob("*") if p.is_file())
        for file_path in files:
            try:
                text = file_path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            findings.extend(scan_text("path", str(file_path), text))
    else:
        findings.extend(scan_worktree(root, allowlist))
        if args.staged:
            findings.extend(scan_staged(root, allowlist))
        if args.history:
            findings.extend(scan_history(root, args.history_limit, allowlist))

    if findings:
        for finding in dict.fromkeys(findings):
            print(f"ERROR: {finding}", file=sys.stderr)
        print(f"Secret scan failed: {len(set(findings))} finding(s)", file=sys.stderr)
        return 2
    if args.path:
        surfaces = [str(args.path)]
    else:
        surfaces = ["worktree"] + (["index"] if args.staged else []) + (["history"] if args.history else [])
    print(f"Secret scan passed: no findings ({', '.join(surfaces)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
