"""AEGIS-010 — the secret scanner must fire on all three surfaces.

A scanner that only inspects the worktree reports a repository clean while a
private key sits in the index or three commits back in history. Each surface is
tested against a throwaway git repository built inside the test.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

import scan_secrets

pytestmark = pytest.mark.unit

FIXTURES = Path(__file__).parent / "fixtures"

# Non-live example values; the shapes are real, the credentials are not.
EXAMPLE_AWS_KEY = "AKIA" + "IOSFODNN7EXAMPLE"
EXAMPLE_PRIVATE_KEY = "-----BEGIN RSA PRIVATE KEY-----\nRVhBTVBMRQ==\n-----END RSA PRIVATE KEY-----\n"


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=root, capture_output=True, text=True, check=True
    ).stdout


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    root.mkdir()
    git(root, "init", "-q")
    git(root, "config", "user.email", "test@example.invalid")
    git(root, "config", "user.name", "AEGIS Test")
    (root / "README.md").write_text("clean\n", encoding="utf-8")
    git(root, "add", "README.md")
    git(root, "commit", "-qm", "initial")
    return root


def test_clean_repository_passes(repo):
    assert scan_secrets.main(["--root", str(repo), "--staged", "--history"]) == 0


def test_worktree_secret_is_detected(repo, capsys):
    (repo / "settings.py").write_text(f'AWS_KEY = "{EXAMPLE_AWS_KEY}"\n', encoding="utf-8")
    git(repo, "add", "settings.py")
    git(repo, "commit", "-qm", "add settings")
    assert scan_secrets.main(["--root", str(repo)]) == 2
    assert "aws-access-key-id" in capsys.readouterr().err


def test_staged_secret_is_detected_before_commit(repo, capsys):
    (repo / "deploy.sh").write_text(f'export GITHUB_TOKEN="{"ghp_" + "A" * 36}"\n', encoding="utf-8")
    git(repo, "add", "deploy.sh")
    assert scan_secrets.main(["--root", str(repo), "--staged"]) == 2
    assert "[index]" in capsys.readouterr().err


def test_history_secret_survives_deletion(repo, capsys):
    key = repo / "id_rsa.bak"
    key.write_text(EXAMPLE_PRIVATE_KEY, encoding="utf-8")
    git(repo, "add", "id_rsa.bak")
    git(repo, "commit", "-qm", "oops")
    key.unlink()
    git(repo, "rm", "-q", "id_rsa.bak")
    git(repo, "commit", "-qm", "remove key")

    # The worktree is genuinely clean now; history is not.
    assert scan_secrets.main(["--root", str(repo)]) == 0
    assert scan_secrets.main(["--root", str(repo), "--history"]) == 2
    assert "private-key-block" in capsys.readouterr().err


def test_tracked_sensitive_path_is_detected(repo, capsys):
    (repo / ".env").write_text("HARMLESS=1\n", encoding="utf-8")
    git(repo, "add", "-f", ".env")
    git(repo, "commit", "-qm", "track dotenv")
    assert scan_secrets.main(["--root", str(repo)]) == 2
    assert "tracked-sensitive-path" in capsys.readouterr().err


def test_env_example_is_permitted(repo):
    (repo / ".env.example").write_text("AEGIS_API_KEY=set-me\n", encoding="utf-8")
    git(repo, "add", ".env.example")
    git(repo, "commit", "-qm", "document env")
    assert scan_secrets.main(["--root", str(repo)]) == 0


def test_negative_fixture_tree_fails(capsys):
    assert scan_secrets.main(["--path", str(FIXTURES / "secrets_bad")]) == 2
    err = capsys.readouterr().err
    assert "private-key-block" in err
    assert "assigned-credential" in err


def test_clean_fixture_tree_passes():
    """Secret-adjacent vocabulary must not be reported; a noisy gate gets disabled."""
    assert scan_secrets.main(["--path", str(FIXTURES / "secrets_ok")]) == 0


def test_live_repository_is_clean(repo_root):
    assert scan_secrets.main(["--root", str(repo_root), "--staged", "--history"]) == 0


def test_allowlist_entries_require_a_reason(repo_root):
    for entry in scan_secrets.load_allowlist(repo_root):
        assert entry["reason"].strip(), entry
