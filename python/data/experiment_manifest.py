"""Experiment manifests: what a run must record to be re-runnable.

The schema is ``configs/schemas/experiment_manifest.v1.json``. M0 delivers the
schema, this builder and its validation; the experiment *registry* — storage,
listing, artifact lookup — is M9 (AEGIS-215).

The distinction matters because the failure this guards against happens at write
time, not at read time. A run that did not record its commit, its resolved
configuration digest, its seed and its rerun command cannot be reproduced later
no matter how good the registry is; by the time anybody notices, the run is
months old and the code has moved on.

Two details are deliberate:

* the **resolved** configuration is hashed, not the file. Environment and CLI
  overrides change what actually ran, and a file digest would silently omit them;
* the git commit records a ``-dirty`` suffix when the tree has uncommitted
  changes. A manifest that names a clean commit for a dirty tree is worse than
  one with no commit at all, because it looks reproducible.
"""

from __future__ import annotations

import json
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from common.clock import Nanos, WallClock

from data.schema_registry import SchemaError, SchemaRegistry

MANIFEST_SCHEMA_NAME = "experiment_manifest"
MANIFEST_SCHEMA_VERSION = 1
MANIFEST_SCHEMA_PATH = "configs/schemas/experiment_manifest.v1.json"


def load_registry(root: Path) -> SchemaRegistry:
    registry = SchemaRegistry()
    registry.register_file(root / MANIFEST_SCHEMA_PATH)
    return registry


def git_commit(root: Path) -> str:
    """Return the current commit, suffixed ``-dirty`` when the tree is modified."""
    def run(*args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=False)

    head = run("rev-parse", "HEAD")
    if head.returncode != 0:
        raise SchemaError(
            "cannot determine the code commit; a run that cannot name the code it executed "
            "is not reproducible (AEGIS-210)"
        )
    commit = head.stdout.strip()
    status = run("status", "--porcelain")
    if status.returncode == 0 and status.stdout.strip():
        # Naming a clean commit for a dirty tree is worse than naming none: it
        # looks reproducible and is not.
        commit += "-dirty"
    return commit


def build_manifest(
    *,
    experiment_id: str,
    code_commit: str,
    config_digest: str,
    seed: int,
    rerun_command: str,
    clock: WallClock,
    data_version: str | None = None,
    date_range: Mapping[str, str] | None = None,
    contracts: Sequence[str] | None = None,
    roll_method: str | None = None,
    costs: Mapping[str, float] | None = None,
    environment: Mapping[str, Any] | None = None,
    artifacts: Sequence[str] | None = None,
) -> dict[str, Any]:
    """Assemble a manifest. Optional fields are omitted rather than left empty.

    An empty ``roll_method`` reads as "no roll method"; an absent one reads as
    "this run had none". The two are different, and research that compares runs
    across roll conventions depends on telling them apart.
    """
    manifest: dict[str, Any] = {
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "experiment_id": experiment_id,
        "code_commit": code_commit,
        "config_digest": config_digest,
        "seed": seed,
        "created_at_ns": Nanos(clock.now_utc()),
        "rerun_command": rerun_command,
    }
    optional: dict[str, Any] = {
        "data_version": data_version,
        "date_range": dict(date_range) if date_range else None,
        "contracts": list(contracts) if contracts else None,
        "roll_method": roll_method,
        "costs": dict(costs) if costs else None,
        "environment": dict(environment) if environment else None,
        "artifacts": list(artifacts) if artifacts else None,
    }
    manifest.update({key: value for key, value in optional.items() if value is not None})
    return manifest


def validate_manifest(registry: SchemaRegistry, manifest: Mapping[str, Any]) -> None:
    registry.validate(MANIFEST_SCHEMA_NAME, manifest)


def to_json(manifest: Mapping[str, Any]) -> str:
    """Serialize deterministically, so two identical runs write identical files."""
    return json.dumps(manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n"


def write_manifest(path: Path, registry: SchemaRegistry, manifest: Mapping[str, Any]) -> None:
    """Validate then write. An invalid manifest never reaches disk.

    Writing first and validating later leaves an unusable artifact behind that
    somebody will find and trust.
    """
    validate_manifest(registry, manifest)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(to_json(manifest), encoding="utf-8")


def read_manifest(path: Path, registry: SchemaRegistry) -> dict[str, Any]:
    document: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    validate_manifest(registry, document)
    return document
