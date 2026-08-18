"""AEGIS-227 (M5 addendum) -- the parallelised clang-tidy fan-out in
scripts/check_cpp_style.sh must (a) still cover every file the sequential
git-ls-files enumeration would have, and (b) still fail the script the
moment any one worker fails. Both are proven against a throwaway fixture
repo with stubbed clang-format/clang-tidy, not the real toolchain -- this
test is about the script's fan-out logic, not clang-tidy's own diagnostics.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.unit

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_cpp_style.sh"


def _build_fixture_repo(tmp_path: Path) -> Path:
    """A tiny real git repo (git ls-files needs one) with three tracked
    C++ sources and a stand-in compile_commands.json."""
    repo = tmp_path / "fixture_repo"
    (repo / "cpp").mkdir(parents=True)
    (repo / "build" / "debug").mkdir(parents=True)
    (repo / "scripts").mkdir()

    for name in ("a.cpp", "b.cpp", "c.cpp"):
        (repo / "cpp" / name).write_text("int x;\n")
    (repo / "build" / "debug" / "compile_commands.json").write_text("[]\n")
    shutil.copy(SCRIPT, repo / "scripts" / "check_cpp_style.sh")

    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.name", "test"], cwd=repo, check=True)
    subprocess.run(["git", "add", "-A"], cwd=repo, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "fixture"], cwd=repo, check=True)
    return repo


def _install_stub_bin(tmp_path: Path, *, fail_on: str | None) -> Path:
    """A PATH directory shadowing clang-format (always passes) and
    clang-tidy (records every filename it was invoked on to a log; fails
    only when invoked on `fail_on`, if given)."""
    bin_dir = tmp_path / "stub_bin"
    bin_dir.mkdir()
    (bin_dir / "clang-format").write_text("#!/usr/bin/env bash\nexit 0\n")
    log_path = tmp_path / "clang_tidy_invocations.log"
    fail_clause = f'[[ "$file" == *"{fail_on}" ]] && exit 1' if fail_on else "true"
    (bin_dir / "clang-tidy").write_text(
        f"""#!/usr/bin/env bash
file="${{@: -1}}"
echo "$file" >> "{log_path}"
{fail_clause}
exit 0
"""
    )
    for tool in ("clang-format", "clang-tidy"):
        (bin_dir / tool).chmod(0o755)
    return bin_dir


def _run_script(repo: Path, bin_dir: Path, *, jobs: str = "2") -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["PATH"] = f"{bin_dir}:{env['PATH']}"
    env["AEGIS_TIDY_JOBS"] = jobs
    return subprocess.run(
        ["bash", "scripts/check_cpp_style.sh", "build/debug"],
        cwd=repo,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )


def test_parallel_fan_out_covers_every_tracked_source(tmp_path: Path) -> None:
    repo = _build_fixture_repo(tmp_path)
    bin_dir = _install_stub_bin(tmp_path, fail_on=None)

    result = _run_script(repo, bin_dir)

    assert result.returncode == 0, result.stderr
    invoked = sorted(Path(line).name for line in (tmp_path / "clang_tidy_invocations.log").read_text().splitlines())
    assert invoked == ["a.cpp", "b.cpp", "c.cpp"]


def test_a_single_worker_failure_fails_the_whole_script(tmp_path: Path) -> None:
    repo = _build_fixture_repo(tmp_path)
    bin_dir = _install_stub_bin(tmp_path, fail_on="b.cpp")

    result = _run_script(repo, bin_dir)

    assert result.returncode != 0
    # Even though one worker failed, the fan-out still dispatched every file
    # -- xargs does not stop launching remaining jobs on a single failure by
    # default, and this test pins that behaviour rather than assuming it.
    invoked = sorted(Path(line).name for line in (tmp_path / "clang_tidy_invocations.log").read_text().splitlines())
    assert invoked == ["a.cpp", "b.cpp", "c.cpp"]
