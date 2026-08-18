"""Shared deterministic M4 report foundation (AEGIS-079, AEGIS-081, AEGIS-024).

Every M4 report (stationarity, roll/expiry attribution, roll-method
sensitivity) is built by attaching report-specific ``findings`` to a
:class:`ReportProvenance` this module produces -- experiment metadata, input
provenance (path *and* content digest, so a report's inputs are pinned, not
just named), strategy configuration, and dataset/roll-policy identity.
Reuses ``data.experiment_manifest.git_commit`` for the code-commit half
rather than duplicating it.

Serialization is deterministic (``sort_keys=True``, fixed separators) so two
renders of the same inputs are byte-identical --
``tests/unit/test_report_model.py`` checks this directly, and that a mutated
input changes its recorded digest.
"""

from __future__ import annotations

import hashlib
import json
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Any

from data.experiment_manifest import git_commit

__all__ = [
    "InputProvenance",
    "ReportProvenance",
    "build_report_provenance",
    "hash_file",
    "render_report",
]


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
        code_commit=git_commit(root),
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
