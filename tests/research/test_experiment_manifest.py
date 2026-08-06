"""AEGIS-097, AEGIS-209 through AEGIS-216 — a run must record enough to be re-run.

This is the research layer's content at M0. There is no strategy to reproduce
yet, so what is tested is the thing that makes reproduction possible later: the
manifest a run writes. The failure it guards against happens at write time — a
run that did not record its commit, resolved-config digest, seed and rerun
command cannot be reproduced afterwards no matter how good the registry is, and
by the time anybody notices, the code has moved on.

The experiment *registry* (storage, listing, artifact lookup) is M9 work.
"""

from __future__ import annotations

import json
import subprocess

import pytest
from common.clock import ManualClock
from common.config import resolve
from data.experiment_manifest import (
    build_manifest,
    git_commit,
    load_registry,
    read_manifest,
    to_json,
    write_manifest,
)
from data.schema_registry import SchemaError

pytestmark = pytest.mark.research

CLOCK_NANOS = 1_700_000_000_000_000_000
DIGEST = "a" * 64


@pytest.fixture
def registry(repo_root):
    return load_registry(repo_root)


@pytest.fixture
def manifest():
    return build_manifest(
        experiment_id="m0-manifest",
        code_commit="0123456abcdef",
        config_digest=DIGEST,
        seed=42,
        rerun_command="python3 tools/determinism_check.py --seed 42",
        clock=ManualClock(CLOCK_NANOS),
    )


def test_a_minimal_manifest_validates(registry, manifest):
    registry.validate("experiment_manifest", manifest)


@pytest.mark.parametrize(
    "field",
    ["experiment_id", "code_commit", "config_digest", "seed", "created_at_ns", "rerun_command"],
)
def test_every_required_field_is_actually_required(registry, manifest, field):
    """Each of these is something a later reader cannot recover from anywhere else."""
    incomplete = {k: v for k, v in manifest.items() if k != field}
    with pytest.raises(SchemaError, match=field):
        registry.validate("experiment_manifest", incomplete)


def test_a_malformed_config_digest_is_refused(registry, manifest):
    """A digest that is not a SHA-256 cannot have come from a resolved config."""
    manifest["config_digest"] = "not-a-digest"
    with pytest.raises(SchemaError, match="config_digest"):
        registry.validate("experiment_manifest", manifest)


def test_optional_fields_are_absent_rather_than_empty(manifest):
    """An empty roll_method reads as "no roll method"; an absent one reads as
    "this run had none", and research comparing roll conventions needs both."""
    assert "roll_method" not in manifest
    assert "costs" not in manifest
    assert "data_version" not in manifest


def test_a_full_manifest_carries_the_research_provenance(registry):
    full = build_manifest(
        experiment_id="m0-manifest-full",
        code_commit="0123456abcdef",
        config_digest=DIGEST,
        seed=7,
        rerun_command="python3 -m research.calendar_spread --config configs/run.json",
        clock=ManualClock(CLOCK_NANOS),
        data_version="cme-es-futures-1min@2026-01-15",
        date_range={"start": "2024-01-02", "end": "2025-12-31"},
        contracts=["ESH5", "ESM5"],
        roll_method="volume_crossover",
        costs={"fees_bps": 0.2, "spread_bps": 0.5, "slippage_bps": 0.3},
        environment={"python": "3.14.4", "compiler": "Clang 21.1.8"},
        artifacts=["experiments/evidence/AEGIS-005/summary.json"],
    )
    registry.validate("experiment_manifest", full)
    assert full["roll_method"] == "volume_crossover"
    assert full["costs"]["fees_bps"] == 0.2


def test_serialization_is_deterministic(manifest):
    """Two identical runs must write identical manifest files."""
    assert to_json(manifest) == to_json(dict(reversed(list(manifest.items()))))


def test_an_invalid_manifest_never_reaches_disk(registry, tmp_path, manifest):
    """Writing first and validating later leaves an unusable artifact somebody
    will later find and trust."""
    manifest["seed"] = -1
    path = tmp_path / "manifest.json"
    with pytest.raises(SchemaError):
        write_manifest(path, registry, manifest)
    assert not path.exists()


def test_write_then_read_round_trips(registry, tmp_path, manifest):
    path = tmp_path / "nested/manifest.json"
    write_manifest(path, registry, manifest)
    assert read_manifest(path, registry) == manifest


def test_reading_an_invalid_manifest_fails_rather_than_returning_it(registry, tmp_path):
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps({"schema_version": 1, "experiment_id": "x"}), encoding="utf-8")
    with pytest.raises(SchemaError):
        read_manifest(path, registry)


def test_git_commit_marks_a_dirty_tree(tmp_path):
    """Naming a clean commit for a dirty tree is worse than naming none: it looks
    reproducible and is not."""
    root = tmp_path / "repo"
    root.mkdir()

    def git(*args):
        return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=True)

    git("init", "-q")
    git("config", "user.email", "test@example.invalid")
    git("config", "user.name", "AEGIS Test")
    (root / "a.txt").write_text("one\n", encoding="utf-8")
    git("add", "-A")
    git("commit", "-qm", "initial")

    clean = git_commit(root)
    assert not clean.endswith("-dirty")

    (root / "a.txt").write_text("two\n", encoding="utf-8")
    assert git_commit(root).endswith("-dirty")


def test_git_commit_fails_loudly_outside_a_repository(tmp_path):
    with pytest.raises(SchemaError, match="not reproducible"):
        git_commit(tmp_path)


def test_the_manifest_digest_matches_the_resolved_configuration(repo_root, registry):
    """The resolved config is hashed, not the file: environment and CLI overrides
    change what actually ran, and a file digest would omit them."""
    config_path = repo_root / "tests/unit/fixtures/configs/valid/full.json"
    plain = resolve(repo_root, path=config_path, environ={}, defaults={})
    overridden = resolve(repo_root, path=config_path, environ={"AEGIS_RUN__SEED": "999"}, defaults={})
    assert plain.digest() != overridden.digest()

    manifest = build_manifest(
        experiment_id=overridden.experiment_id,
        code_commit="0123456abcdef",
        config_digest=overridden.digest(),
        seed=int(overridden.get("run.seed")),
        rerun_command="python3 tools/determinism_check.py",
        clock=ManualClock(CLOCK_NANOS),
    )
    registry.validate("experiment_manifest", manifest)
    assert manifest["seed"] == 999


def test_the_live_repository_can_produce_a_valid_manifest(repo_root, registry):
    """End to end over the real repository: commit, resolved config, seed, rerun."""
    resolved = resolve(
        repo_root,
        path=repo_root / "tests/unit/fixtures/configs/valid/minimal.json",
        environ={},
        defaults={},
    )
    manifest = build_manifest(
        experiment_id=resolved.experiment_id,
        code_commit=git_commit(repo_root),
        config_digest=resolved.digest(),
        seed=int(resolved.get("run.seed")),
        rerun_command="bash scripts/ci_local.sh",
        clock=ManualClock(CLOCK_NANOS),
    )
    registry.validate("experiment_manifest", manifest)
