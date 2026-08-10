#!/usr/bin/env python3
"""One provenance stamp for every evidence artifact (AEGIS-003).

Seven generators had each grown their own private ``_provenance()``. They
agreed by accident, which is the condition under which they eventually stop
agreeing, and the duplication hid a real problem in what ``dirty`` meant.

**What ``dirty`` answers.** The question an evidence reader has is "can I
rebuild this artifact from the named commit?" -- that is, *was the code that
produced this artifact committed*. The naive answer, ``git status --porcelain``
is non-empty, cannot answer it during a batch regeneration: the first generator
writes its file, and every generator after it sees a dirty tree caused entirely
by evidence output that is itself being regenerated. M2's slice 3-7 artifacts
carry exactly that artefact -- ``dirty: true`` at a commit whose *code* was
clean, recording nothing except the order the generators happened to run in.

So ``dirty`` here excludes changes under ``experiments/evidence/``. A modified
tool, module, config or fixture still makes it true; a sibling artifact written
one second earlier does not. That is the honest reading, and it is the one the
field is used for.

Nothing here weakens the check: :func:`provenance` still reports ``dirty: true``
whenever any producing code differs from the recorded commit, which is the case
a reader must not be misled about.
"""

from __future__ import annotations

import subprocess
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]

# Evidence output is the generators' own product, not drift in the code under
# test. Excluded from the dirty computation; see the module docstring.
EVIDENCE_PREFIX = "experiments/evidence/"


def _git(*args: str, root: Path = ROOT) -> str:
    result = subprocess.run(
        ["git", *args], cwd=root, capture_output=True, text=True, check=False
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def changed_paths(root: Path = ROOT) -> list[str]:
    """Every uncommitted path, as bare repository-relative paths.

    Two commands that emit *only* paths, rather than ``git status
    --porcelain``, whose ``XY <path>`` status column has to be sliced off by
    width -- and which is trivially mis-sliced the moment anything strips the
    leading space of an unstaged-modification line. Renames appear here as
    both their old and new path, which is what we want: a rename out of
    evidence into code is a code change.
    """
    tracked = _git("diff", "--name-only", "HEAD", root=root).splitlines()
    untracked = _git("ls-files", "--others", "--exclude-standard", root=root).splitlines()
    return [path for path in (*tracked, *untracked) if path.strip()]


def code_is_dirty(root: Path = ROOT) -> bool:
    """True when anything *other than* evidence output is uncommitted."""
    return any(not path.startswith(EVIDENCE_PREFIX) for path in changed_paths(root))


def provenance(root: Path = ROOT) -> dict[str, Any]:
    """The provenance block every evidence artifact carries."""
    return {
        "generated_on": datetime.now(UTC).strftime("%Y-%m-%d"),
        "repository_commit": _git("rev-parse", "HEAD", root=root),
        "dirty": code_is_dirty(root),
        "dirty_means": (
            "code outside experiments/evidence/ was uncommitted when this artifact "
            "was generated; sibling evidence written in the same batch is excluded "
            "(tools/evidence_provenance.py)"
        ),
    }
