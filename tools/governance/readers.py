#!/usr/bin/env python3
"""Ways to read a candidate branch **as data** (R8, ADR-0014).

The authoritative gate judges a pull request without ever executing it. That is
the whole reason these exist: the privileged ``pull_request_target`` job runs
from the base checkout, and the candidate's files reach it only through one of
these readers — as bytes to be hashed and parsed, never as code to be imported,
sourced or run.

Three implementations, one protocol:

* :class:`GitHubApiReader` — CI. Reads through the GitHub API, so the candidate
  tree is never placed on the runner's disk at all.
* :class:`GitShowReader` — local preflight. ``git show <sha>:<path>`` reads a
  blob out of the object database without checking anything out.
* :class:`MappingReader` — tests. A dict, so every attack case in
  ``tests/unit/test_authoritative_governance.py`` runs with no network and no
  GitHub.

A path that does not exist reads as ``None``. Callers must distinguish that
from an empty file: "deleted" and "emptied" are different diffs.
"""

from __future__ import annotations

import base64
import json
import subprocess
from collections.abc import Mapping, Sequence
from typing import Protocol


class CandidateReader(Protocol):
    """Read a file from the candidate revision, or ``None`` if it is absent."""

    def read_text(self, path: str) -> str | None: ...


class MappingReader:
    """A candidate tree held in memory. Used by the test suite."""

    def __init__(self, files: Mapping[str, str]) -> None:
        self._files = dict(files)

    def read_text(self, path: str) -> str | None:
        return self._files.get(path)


class DirectoryReader:
    """A candidate tree held in a plain directory.

    This is what the committed counter-example fixture uses, so the negative
    gate is an ordinary command rather than a bespoke harness — the same shape
    as the ``arch_violation`` and ``claims_bad`` fixtures.
    """

    def __init__(self, root: str) -> None:
        from pathlib import Path

        self._root = Path(root)

    def read_text(self, path: str) -> str | None:
        candidate = self._root / path
        # Refuse to escape the fixture root: a fixture must not be able to read
        # the real repository and accidentally pass by borrowing its files.
        try:
            candidate.resolve().relative_to(self._root.resolve())
        except ValueError:
            return None
        if not candidate.is_file():
            return None
        return candidate.read_text(encoding="utf-8")


class GitShowReader:
    """Read blobs out of a local object database at a fixed revision.

    ``git show`` does not check anything out and does not run repository hooks,
    so a hostile candidate tree cannot execute during a preflight run.
    """

    def __init__(self, repo: str, revision: str) -> None:
        self._repo = repo
        self._revision = revision

    def read_text(self, path: str) -> str | None:
        result = subprocess.run(
            ["git", "-C", self._repo, "show", f"{self._revision}:{path}"],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            return None
        return result.stdout

    def changed_files(self, base: str) -> list[str]:
        merge_base = subprocess.run(
            ["git", "-C", self._repo, "merge-base", base, self._revision],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        diff = subprocess.run(
            ["git", "-C", self._repo, "diff", "--name-only", merge_base, self._revision],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        return sorted({line for line in diff.splitlines() if line})


class GitHubApiReader:
    """Read a pull request's head through the GitHub API.

    Nothing is cloned and nothing is written to disk. ``gh`` is invoked with an
    explicit argument list, so no shell interpolation of candidate-controlled
    data is possible.
    """

    def __init__(self, repo: str, head_sha: str, pull_number: int | None = None) -> None:
        self._repo = repo
        self._head_sha = head_sha
        self._pull_number = pull_number

    def _gh(self, *args: str) -> tuple[int, str]:
        result = subprocess.run(["gh", *args], capture_output=True, text=True, check=False)
        return result.returncode, result.stdout

    def read_text(self, path: str) -> str | None:
        code, out = self._gh(
            "api",
            "-H",
            "Accept: application/vnd.github+json",
            f"repos/{self._repo}/contents/{path}?ref={self._head_sha}",
        )
        if code != 0 or not out.strip():
            return None
        try:
            payload = json.loads(out)
        except json.JSONDecodeError:
            return None
        if payload.get("encoding") != "base64" or "content" not in payload:
            return None
        raw = base64.b64decode(payload["content"])
        return raw.decode("utf-8", errors="replace")

    def changed_files(self) -> Sequence[str]:
        if self._pull_number is None:
            raise ValueError("changed_files() needs a pull request number")
        code, out = self._gh(
            "api",
            "--paginate",
            "-H",
            "Accept: application/vnd.github+json",
            f"repos/{self._repo}/pulls/{self._pull_number}/files",
            "--jq",
            ".[].filename",
        )
        if code != 0:
            # Fail closed: an unreadable file list must never be read as
            # "nothing changed", which would pass every check vacuously.
            raise RuntimeError(
                f"could not list files for {self._repo}#{self._pull_number}; refusing to continue"
            )
        return sorted({line.strip() for line in out.splitlines() if line.strip()})
