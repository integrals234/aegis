"""Shared fixtures and import paths for the AEGIS test layers."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "python"))


@pytest.fixture
def repo_root() -> Path:
    return ROOT


@pytest.fixture
def catalogue(tmp_path: Path):
    """Build a miniature but structurally valid requirement catalogue on disk.

    The auditor is tested against fixtures rather than the live repository, so a
    test can prove the auditor *rejects* something without mutating real files.
    """

    from audit_requirements import REQUIRED_MODULES

    def build(requirements: list[dict], statuses: dict[str, dict], frozen: dict[str, str] | None = None) -> Path:
        root = tmp_path / "repo"
        (root / "requirements").mkdir(parents=True, exist_ok=True)

        # Every required module must be present or the audit fails for an
        # unrelated reason; pad with placeholder entries in later milestones.
        used = {r["module"] for r in requirements}
        padded = list(requirements)
        padded_statuses = dict(statuses)
        for index, module in enumerate(sorted(REQUIRED_MODULES - used)):
            rid = f"PAD-{index:03d}"
            padded.append(
                {
                    "id": rid,
                    "module": module,
                    "milestone": "M9",
                    "priority": "must",
                    "title": module,
                    "description": module,
                    "acceptance": module,
                }
            )
            padded_statuses[rid] = {"status": "not_started", "implementation": [], "tests": [], "reports": []}

        (root / "requirements/requirements.json").write_text(
            json.dumps({"requirements": padded}, indent=2), encoding="utf-8"
        )
        (root / "requirements/implementation_status.json").write_text(
            json.dumps({"requirements": padded_statuses}, indent=2), encoding="utf-8"
        )
        (root / "requirements/frozen_hashes.json").write_text(
            json.dumps(frozen or {}, indent=2), encoding="utf-8"
        )
        return root

    return build
