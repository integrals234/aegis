"""AEGIS-010 — assert the project settings deny sensitive paths.

This is a **configuration assertion**, not a behavioural proof. It checks that
``.claude/settings.json`` declares the deny rules; whether the harness honours
them is a property of Claude Code, not of this repository. The behavioural
control for committed secrets is ``tools/scan_secrets.py``.
"""

from __future__ import annotations

import json

import pytest

pytestmark = pytest.mark.unit

REQUIRED_DENY_TARGETS = (
    "**/.env",
    "**/.env.*",
    "**/secrets/**",
    "**/credentials/**",
    "**/private/**",
)


@pytest.fixture
def settings(repo_root):
    return json.loads((repo_root / ".claude/settings.json").read_text(encoding="utf-8"))


def test_read_access_to_sensitive_paths_is_denied(settings):
    deny = set(settings["permissions"]["deny"])
    for target in REQUIRED_DENY_TARGETS:
        assert f"Read({target})" in deny, f"Read({target}) is not denied"


def test_write_access_to_sensitive_paths_is_denied(settings):
    deny = set(settings["permissions"]["deny"])
    for target in ("**/.env", "**/secrets/**", "**/credentials/**", "**/private/**"):
        assert f"Write({target})" in deny, f"Write({target}) is not denied"


def test_key_material_is_denied(settings):
    deny = set(settings["permissions"]["deny"])
    for target in ("**/*.pem", "**/*.key", "**/id_rsa"):
        assert f"Read({target})" in deny, f"Read({target}) is not denied"


def test_frozen_file_hook_is_registered(settings):
    """AEGIS-001: the hook must actually be wired to the write tools."""
    pre = settings["hooks"]["PreToolUse"]
    commands = [h["command"] for entry in pre for h in entry["hooks"]]
    assert any("protect_spec.py" in c for c in commands)
    matchers = [entry["matcher"] for entry in pre]
    assert any("Write" in m and "Edit" in m for m in matchers)


def test_no_allow_rule_grants_blanket_shell_access(settings):
    allow = settings["permissions"]["allow"]
    assert "Bash(*)" not in allow
    assert not any(rule.strip() in ("Bash", "Bash()") for rule in allow)
