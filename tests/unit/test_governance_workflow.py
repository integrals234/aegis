"""The authoritative gate's workflow must stay authoritative (R8, ADR-0014).

These assertions look pedantic and are not. Each one corresponds to a way the
trust boundary silently stops being a trust boundary while every check still
reports green:

* switch the event to ``pull_request`` and the workflow file comes from the
  head, so a candidate can keep the job name — and with it the required status
  check — while replacing the body with ``exit 0``;
* add ``ref:`` to the checkout and the privileged job starts running candidate
  code;
* rename the job and the required status check silently stops being required,
  because the ruleset matches it by name.
"""

from __future__ import annotations

import pytest
import yaml

pytestmark = pytest.mark.unit

WORKFLOW = ".github/workflows/governance.yml"

# The exact string registered as a required status check in the "Protect main"
# ruleset. Renaming the job drops the requirement without failing anything.
REQUIRED_CHECK_CONTEXT = "Authoritative governance gate (R8)"


@pytest.fixture
def workflow(repo_root):
    return yaml.safe_load((repo_root / WORKFLOW).read_text(encoding="utf-8"))


def test_uses_pull_request_target_not_pull_request(workflow):
    # PyYAML parses a bare `on:` key as the boolean True.
    triggers = workflow.get("on") or workflow.get(True)
    assert "pull_request_target" in triggers, (
        "under pull_request the workflow file comes from the candidate's head, "
        "so the candidate could neuter this job while keeping its name"
    )
    assert "pull_request" not in triggers


def test_job_name_matches_the_required_status_check(workflow):
    assert workflow["jobs"]["authoritative"]["name"] == REQUIRED_CHECK_CONTEXT


def test_permissions_are_read_only(workflow):
    assert workflow["permissions"] == {"contents": "read", "pull-requests": "read"}


def test_checkout_never_takes_the_candidate_head(workflow):
    """The default for pull_request_target is the base branch. Naming a ref is
    how a privileged job accidentally starts executing the pull request."""
    steps = workflow["jobs"]["authoritative"]["steps"]
    checkouts = [s for s in steps if str(s.get("uses", "")).startswith("actions/checkout")]
    assert checkouts, "the gate must check out the trusted base tree"
    for checkout in checkouts:
        assert "ref" not in (checkout.get("with") or {}), (
            "checking out the candidate head in a privileged job is the classic "
            "pwn-request; the base ref default is deliberate"
        )


def test_the_gate_is_pointed_at_the_trusted_tree(repo_root):
    text = (repo_root / WORKFLOW).read_text(encoding="utf-8")
    assert "--trusted-root \"$GITHUB_WORKSPACE\"" in text, (
        "the checker must judge from the base checkout; passing the candidate's "
        "own tree would reintroduce the circularity R8 exists to remove"
    )
    assert "tools/governance/authoritative_check.py" in text


def test_the_negative_gate_is_present(repo_root):
    """A gate that cannot fail proves nothing."""
    text = (repo_root / WORKFLOW).read_text(encoding="utf-8")
    assert "governance_tampered" in text


def test_the_advisory_checkers_say_they_are_advisory(repo_root):
    """A future reader must not mistake either for the control of record."""
    for tool in ("tools/check_scope.py", "tools/check_frozen.py"):
        text = (repo_root / tool).read_text(encoding="utf-8")
        assert "ADVISORY" in text, f"{tool} must not read as the trust boundary"
        assert "Authoritative governance gate (R8)" in text


def test_policy_declares_the_governance_paths_that_protect_it(repo_root):
    """The policy must govern the files that decide who may change what —
    including, specifically, the two advisory checkers and the gate itself."""
    policy = yaml.safe_load(
        (repo_root / "configs/governance/policy.yaml").read_text(encoding="utf-8")
    )
    governed = set(policy["governance_paths"])
    for required in (
        "configs/milestone_scope.yaml",
        "requirements/frozen_hashes.json",
        ".github/workflows/governance.yml",
        "tools/governance/**",
        "tools/check_scope.py",
        "tools/check_frozen.py",
    ):
        assert required in governed, f"{required} must require an owner approval to change"
