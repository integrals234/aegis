"""AEGIS-234 — the CI workflow and the local gate matrix must not drift apart.

A workflow that has quietly diverged from ``scripts/ci_local.sh`` is worse than
no workflow: a green tick then means something different from what a developer
just ran, and the difference is discovered when a merge breaks main.

These tests do not run CI. They check that every gate this milestone built is
invoked by both, which is the part that rots silently.
"""

from __future__ import annotations

import re

import pytest
import yaml

pytestmark = pytest.mark.unit

# Every tool that gates something. A tool absent from either side is a control
# somebody built and nothing runs.
GATE_TOOLS = (
    "tools/audit_requirements.py",
    "tools/check_frozen.py",
    "tools/check_scope.py",
    "tools/check_architecture.py",
    "tools/check_claims.py",
    "tools/check_adrs.py",
    "tools/check_docs.py",
    "tools/scan_secrets.py",
    "tools/check_sample_data.py",
    "tools/check_pack_manifest.py",
    "tools/generate_traceability.py",
    "tools/generate_deferred_register.py",
    "tools/determinism_check.py",
    "tools/run_test_layers.py",
)

PRESETS = ("debug", "release", "asan-ubsan")


@pytest.fixture
def workflow_text(repo_root):
    return (repo_root / ".github/workflows/ci.yml").read_text(encoding="utf-8")


@pytest.fixture
def local_text(repo_root):
    return (repo_root / "scripts/ci_local.sh").read_text(encoding="utf-8")


@pytest.mark.parametrize("tool", GATE_TOOLS)
def test_every_gate_runs_in_ci(workflow_text, tool):
    assert tool in workflow_text, f"{tool} is never invoked by the CI workflow"


@pytest.mark.parametrize("tool", GATE_TOOLS)
def test_every_gate_runs_locally(local_text, tool):
    assert tool in local_text, f"{tool} is never invoked by scripts/ci_local.sh"


@pytest.mark.parametrize("preset", PRESETS)
def test_every_cmake_preset_is_built_in_ci(workflow_text, preset):
    assert preset in workflow_text


@pytest.mark.parametrize("preset", PRESETS)
def test_every_cmake_preset_is_built_locally(local_text, preset):
    assert preset in local_text


def test_workflow_is_valid_yaml(repo_root):
    document = yaml.safe_load((repo_root / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
    assert document["name"] == "AEGIS CI"
    assert set(document["jobs"]) >= {"governance", "secrets", "python", "cpp", "determinism"}


def test_history_dependent_gates_use_full_history(repo_root):
    """A shallow clone makes the frozen-file and scope gates pass vacuously."""
    text = (repo_root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    document = yaml.safe_load(text)
    for job_name in ("governance", "secrets"):
        steps = document["jobs"][job_name]["steps"]
        checkout = next(s for s in steps if str(s.get("uses", "")).startswith("actions/checkout"))
        assert checkout["with"]["fetch-depth"] == 0, job_name


def test_ci_pins_the_python_matrix_it_claims_to_support(repo_root):
    document = yaml.safe_load((repo_root / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
    versions = document["jobs"]["python"]["strategy"]["matrix"]["python-version"]
    assert "3.12" in versions, "3.12 is the declared floor in docs/ENVIRONMENT.md"

    probed = (repo_root / "experiments/evidence/AEGIS-228/dependency_probe.json").read_text(
        encoding="utf-8"
    )
    for version in versions:
        assert f'"{version}"' in probed, (
            f"CI tests {version} but no dependency probe covers it; the pins may not install there"
        )


def test_ci_installs_dependencies_with_hashes(workflow_text):
    """Without --require-hashes a re-uploaded wheel installs silently."""
    installs = re.findall(r"pip[\"']? install[^\n]*requirements\.lock", workflow_text)
    assert installs, "CI never installs the lockfile"
    for install in installs:
        assert "--require-hashes" in install, install


def test_negative_gates_are_exercised_in_ci(workflow_text):
    """A gate that cannot fail proves nothing, so CI checks that each still can."""
    assert "negative-gates" in workflow_text
    assert "arch_violation" in workflow_text
    assert "claims_bad" in workflow_text
    assert "secrets_bad" in workflow_text
    assert "--expect-failure" in workflow_text


def test_ci_does_not_claim_to_satisfy_its_own_acceptance(workflow_text, local_text):
    """AEGIS-234's acceptance names a passing workflow on the protected default
    branch. Only a real run can satisfy that, and both files say so."""
    assert "registered verification obligation" in workflow_text
    assert "registered verification obligation" in local_text
    assert "Only a real CI run can satisfy that" in local_text
