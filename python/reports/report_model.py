"""Shared deterministic M4/M5 report foundation (AEGIS-079, AEGIS-081,
AEGIS-024).

Every M4/M5 report (stationarity, roll/expiry attribution, roll-method
sensitivity, M5 validation/rejection/portfolio-risk) is built by attaching
report-specific ``findings`` to a :class:`ReportProvenance` this module
produces -- experiment metadata, input provenance (path *and* content
digest, so a report's inputs are pinned, not just named), strategy
configuration, and dataset/roll-policy identity.

Serialization is deterministic (``sort_keys=True``, fixed separators) so two
renders of the same inputs are byte-identical --
``tests/unit/test_report_model.py`` checks this directly, and that a mutated
input changes its recorded digest.

# The M5 fix: sibling-evidence exclusion for ``code_commit``

Before M5, ``code_commit`` came from ``data.experiment_manifest.git_commit``,
whose ``-dirty`` suffix reads plain ``git status --porcelain`` -- which
counts a sibling evidence artifact this same batch just wrote as "the tree
is dirty", even though nothing under ``experiments/evidence/`` bears on
whether the *code* that produced this report is reproducible. That is
exactly the failure ``tools/evidence_provenance.py`` was built to avoid
(AEGIS-003), and M4's accepted residual carried this file as the one spot
that still had it. :func:`_code_commit` below mirrors that module's
exclusion rule -- a change under ``experiments/evidence/`` never marks the
commit dirty; anything else still does -- without importing across the
``python/`` <-> ``tools/`` boundary that would make this module depend on
where a caller's ``sys.path`` happens to include ``tools/``.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Any

__all__ = [
    "InputProvenance",
    "ReportProvenance",
    "build_report_provenance",
    "hash_file",
    "render_report",
]


# Mirrors tools/evidence_provenance.py's EVIDENCE_PREFIX exactly -- both must
# agree on what "sibling evidence, not code" means, or the two dirty
# computations would silently diverge again.
_EVIDENCE_PREFIX = "experiments/evidence/"


def _git(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=False)


def _changed_paths(root: Path) -> list[str]:
    # Every uncommitted repository-relative path (tracked changes plus
    # untracked files) -- the same two-command construction
    # tools/evidence_provenance.py uses, deliberately not `git status
    # --porcelain`, whose status-column width a leading-space-stripped line
    # can mis-slice.
    tracked = _git(root, "diff", "--name-only", "HEAD").stdout.splitlines()
    untracked = _git(root, "ls-files", "--others", "--exclude-standard").stdout.splitlines()
    return [path for path in (*tracked, *untracked) if path.strip()]


def _code_commit(root: Path) -> str:
    # The current commit, suffixed `-dirty` only when something OTHER than
    # experiments/evidence/** is uncommitted -- sibling evidence written
    # earlier in the same generation batch never triggers the suffix.
    head = _git(root, "rev-parse", "HEAD")
    if head.returncode != 0:
        raise RuntimeError(
            "cannot determine the code commit; a report that cannot name the code it was "
            "generated from is not reproducible"
        )
    commit = head.stdout.strip()
    if any(not path.startswith(_EVIDENCE_PREFIX) for path in _changed_paths(root)):
        commit += "-dirty"
    return commit


def hash_file(path: Path) -> str:
    """SHA-256 of ``path``'s bytes, hex-encoded -- the content digest that
    makes a report's provenance verifiable against the actual committed
    file, not just its name."""
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


@dataclass(frozen=True, slots=True)
class InputProvenance:
    """One input dataset's identity: where it is and what it actually
    contained (``content_sha256``) when this report was built -- a path
    alone is not provenance, since the file behind it can change."""

    path: str
    content_sha256: str

    @classmethod
    def from_path(cls, root: Path, relative_path: str) -> InputProvenance:
        return cls(path=relative_path, content_sha256=hash_file(root / relative_path))


@dataclass(frozen=True, slots=True)
class ReportProvenance:
    """Experiment metadata, input provenance, strategy configuration and
    dataset/roll-policy identity -- everything AEGIS-079/081/024's frozen
    acceptance criteria require a report to disclose, gathered once here
    instead of once per report."""

    report_id: str
    code_commit: str
    inputs: tuple[InputProvenance, ...]
    strategy_config: Mapping[str, Any]
    dataset_id: str
    roll_policy_name: str


def build_report_provenance(
    *,
    report_id: str,
    root: Path,
    input_paths: Sequence[str],
    strategy_config: Mapping[str, Any],
    dataset_id: str,
    roll_policy_name: str,
) -> ReportProvenance:
    return ReportProvenance(
        report_id=report_id,
        code_commit=_code_commit(root),
        inputs=tuple(InputProvenance.from_path(root, path) for path in input_paths),
        strategy_config=dict(strategy_config),
        dataset_id=dataset_id,
        roll_policy_name=roll_policy_name,
    )


def _json_default(value: Any) -> Any:
    # Decimal has no native JSON representation; str() is exact and
    # round-trips through Decimal(str(...)) -- never float(), which would
    # silently reintroduce binary rounding into a report.
    if isinstance(value, Decimal):
        return str(value)
    if hasattr(value, "isoformat"):
        return value.isoformat()
    raise TypeError(f"not JSON serializable: {type(value).__name__}")


def render_report(provenance: ReportProvenance, findings: Mapping[str, Any]) -> str:
    """Deterministic canonical JSON (``sort_keys=True``, fixed separators):
    two renders of the same ``provenance``/``findings`` are byte-identical.
    """
    document = {
        "report_id": provenance.report_id,
        "code_commit": provenance.code_commit,
        "inputs": [
            {"path": item.path, "content_sha256": item.content_sha256}
            for item in provenance.inputs
        ],
        "strategy_config": provenance.strategy_config,
        "dataset_id": provenance.dataset_id,
        "roll_policy_name": provenance.roll_policy_name,
        "findings": findings,
    }
    return json.dumps(document, sort_keys=True, separators=(",", ":"), default=_json_default) + "\n"
