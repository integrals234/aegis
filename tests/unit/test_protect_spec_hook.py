"""AEGIS-001 — the PreToolUse hook must fail closed.

The hook is defence in depth (docs/LIMITATIONS.md records what it cannot see),
but a defence that exits 0 whenever it is confused is not a defence. These tests
drive the hook as a subprocess, exactly as Claude Code invokes it.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.unit

HOOK = Path(__file__).resolve().parents[2] / ".claude/hooks/protect_spec.py"
BLOCK = 2
ALLOW = 0


def run(payload: object, repo_root: Path, env_extra: dict[str, str] | None = None, raw: str | None = None):
    env = {"PATH": "/usr/bin:/bin", "CLAUDE_PROJECT_DIR": str(repo_root)}
    if env_extra is not None:
        env.update(env_extra)
        if env_extra.get("CLAUDE_PROJECT_DIR") == "":
            env.pop("CLAUDE_PROJECT_DIR")
    stdin = raw if raw is not None else json.dumps(payload)
    return subprocess.run(
        [sys.executable, str(HOOK)], input=stdin, capture_output=True, text=True, env=env, check=False
    )


def write_payload(path: str) -> dict:
    return {"tool_name": "Write", "tool_input": {"file_path": path, "content": "x"}}


@pytest.mark.parametrize(
    "frozen",
    [
        "docs/MASTER_SPEC.md",
        "requirements/requirements.json",
        "docs/BENCHMARK_POLICY.md",
        "docs/CV_CLAIMS_POLICY.md",
        "docs/DATA_AND_RESEARCH_POLICY.md",
        "requirements/frozen_hashes.json",
    ],
)
def test_frozen_paths_are_blocked(frozen, repo_root):
    result = run(write_payload(frozen), repo_root)
    assert result.returncode == BLOCK
    assert frozen in result.stderr


def test_absolute_path_to_a_frozen_file_is_blocked(repo_root):
    result = run(write_payload(str(repo_root / "docs/MASTER_SPEC.md")), repo_root)
    assert result.returncode == BLOCK


def test_traversal_path_to_a_frozen_file_is_blocked(repo_root):
    result = run(write_payload("docs/../docs/MASTER_SPEC.md"), repo_root)
    assert result.returncode == BLOCK


def test_nested_edit_payload_is_blocked(repo_root):
    payload = {
        "tool_name": "MultiEdit",
        "tool_input": {"edits": [{"file_path": "docs/ARCHITECTURE.md"}, {"file_path": "docs/MASTER_SPEC.md"}]},
    }
    assert run(payload, repo_root).returncode == BLOCK


def test_ordinary_file_is_allowed(repo_root):
    assert run(write_payload("docs/ARCHITECTURE.md"), repo_root).returncode == ALLOW


def test_path_outside_the_project_is_not_this_hooks_business(repo_root):
    assert run(write_payload("/tmp/scratch.txt"), repo_root).returncode == ALLOW


def test_malformed_payload_fails_closed(repo_root):
    result = run(None, repo_root, raw="{not json")
    assert result.returncode == BLOCK
    assert "could not parse" in result.stderr


def test_empty_payload_fails_closed(repo_root):
    result = run(None, repo_root, raw="")
    assert result.returncode == BLOCK
    assert "no payload" in result.stderr


def test_non_object_payload_fails_closed(repo_root):
    result = run(None, repo_root, raw='["Write", "docs/MASTER_SPEC.md"]')
    assert result.returncode == BLOCK


def test_missing_project_dir_fails_closed(repo_root):
    result = run(write_payload("docs/MASTER_SPEC.md"), repo_root, env_extra={"CLAUDE_PROJECT_DIR": ""})
    assert result.returncode == BLOCK
    assert "CLAUDE_PROJECT_DIR" in result.stderr


def test_owner_escape_hatch_is_honoured(repo_root):
    """The owner may edit the specification deliberately; Claude may not."""
    result = run(write_payload("docs/MASTER_SPEC.md"), repo_root, env_extra={"AEGIS_ALLOW_SPEC_EDIT": "1"})
    assert result.returncode == ALLOW
