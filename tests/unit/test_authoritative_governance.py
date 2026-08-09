"""R8 — the authoritative governance gate (AEGIS-007, AEGIS-001; ADR-0014).

Every test builds two trees: a **trusted root** playing protected `main`, and a
**candidate** playing a pull request head. That separation is the whole design,
so it is also the shape of the tests — a case that accidentally passed the
candidate's own policy as the trusted root would prove nothing.

The cases are the attack table from the plan of record. A-J are the content
boundary. They need no keys, no network and no GitHub.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import pytest
import yaml

pytestmark = pytest.mark.unit

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "governance"))

import authoritative_check  # noqa: E402
from readers import MappingReader  # noqa: E402

FROZEN_DOC = "docs/MASTER_SPEC.md"
FROZEN_TEXT = "# Canonical Master Specification\n\nFrozen.\n"
FROZEN_DIGEST = hashlib.sha256(FROZEN_TEXT.encode("utf-8")).hexdigest()

SCOPE = {
    "scope_version": 1,
    "always_allowed": ["docs/BUILD_STATE.md", "adr/**"],
    "milestones": {
        "M1": {
            "description": "exchange core",
            "allowed": ["cpp/**", "tests/**", "tools/**", "configs/**", "docs/**"],
            "denied": ["dashboard/**"],
        },
        "M2": {
            "description": "futures and replay",
            "allowed": ["python/futures/**", "cpp/replay/**", "tests/**", "docs/**"],
            "denied": ["cpp/exchange/matching/**"],
        },
    },
}

GOVERNANCE_PATHS = [
    "configs/governance/**",
    "configs/milestone_scope.yaml",
    "requirements/frozen_hashes.json",
    ".github/workflows/governance.yml",
    "tools/governance/**",
    "tools/check_scope.py",
    "tools/check_frozen.py",
]


def make_trusted(tmp_path: Path, *, milestone: str = "M1", approvals=None) -> Path:
    root = tmp_path / "trusted"
    (root / "configs/governance").mkdir(parents=True)
    (root / "requirements").mkdir(parents=True)

    (root / "configs/governance/policy.yaml").write_text(
        yaml.safe_dump(
            {
                "policy_version": 1,
                "active_milestone": milestone,
                "governance_paths": GOVERNANCE_PATHS,
                "self_path": "configs/governance/policy.yaml",
                "approvals": approvals or [],
            },
            sort_keys=False,
        ),
        encoding="utf-8",
    )
    (root / "configs/milestone_scope.yaml").write_text(yaml.safe_dump(SCOPE), encoding="utf-8")
    (root / "requirements/frozen_hashes.json").write_text(
        json.dumps({FROZEN_DOC: FROZEN_DIGEST}, indent=2), encoding="utf-8"
    )
    return root


def candidate(milestone: str = "M1", **overrides: str) -> MappingReader:
    files = {
        "docs/BUILD_STATE.md": f"# Build State\n\n- Active milestone: {milestone}\n",
        FROZEN_DOC: FROZEN_TEXT,
    }
    files.update(overrides)
    return MappingReader(files)


def judge(trusted: Path, reader: MappingReader, changed: list[str]) -> list[str]:
    return authoritative_check.run(trusted, reader, changed)


# ------------------------------------------------------------------ A to J


def test_a_build_state_approval_line_grants_nothing(tmp_path):
    """A: the retired channel. Naming a path there must not authorise it."""
    trusted = make_trusted(tmp_path)
    reader = candidate(
        **{
            "docs/BUILD_STATE.md": (
                "# Build State\n\n- Active milestone: M1\n"
                "- Owner-approved scope changes: dashboard/app.py\n"
            )
        }
    )
    errors = judge(trusted, reader, ["dashboard/app.py"])
    assert any("denied while M1 is active" in e for e in errors)


def test_b_widening_the_scope_policy_is_inert_and_flagged(tmp_path):
    """B: the gate reads main's scope policy, and the file is a governance path."""
    trusted = make_trusted(tmp_path)
    reader = candidate(
        **{"configs/milestone_scope.yaml": yaml.safe_dump({"milestones": {"M1": {"allowed": ["**"]}}})}
    )
    errors = judge(trusted, reader, ["configs/milestone_scope.yaml", "dashboard/app.py"])
    assert any("configs/milestone_scope.yaml" in e and "owner approval" in e for e in errors)
    assert any("dashboard/app.py" in e for e in errors), "the widening must not take effect"


def test_c_escalating_the_active_milestone_fails(tmp_path):
    """C: M9 would mean an empty denied list and a tree-wide allowed list."""
    trusted = make_trusted(tmp_path, milestone="M1")
    errors = judge(trusted, candidate(milestone="M9"), ["docs/README.md"])
    assert any("trusted policy on main says 'M1'" in e for e in errors)


def test_c_candidate_policy_is_never_consulted(tmp_path):
    """C: a self-granted approval in the candidate's own policy does nothing."""
    trusted = make_trusted(tmp_path)
    self_granted = yaml.safe_dump(
        {
            "policy_version": 1,
            "active_milestone": "M1",
            "approvals": [
                {"id": "self", "milestone": "M1", "paths": ["dashboard/app.py"], "reason": "no"}
            ],
        }
    )
    reader = candidate(**{"configs/governance/policy.yaml": self_granted})
    errors = judge(trusted, reader, ["configs/governance/policy.yaml", "dashboard/app.py"])
    assert any("dashboard/app.py" in e for e in errors)
    # ...but proposing the change is itself allowed: it is inert until merged.
    assert not any("configs/governance/policy.yaml" in e for e in errors)


def test_d_editing_a_frozen_file_and_its_digest_together_fails(tmp_path):
    """D: the laundering route the original R8 report did not name."""
    trusted = make_trusted(tmp_path)
    edited = "# Canonical Master Specification\n\nEdited.\n"
    reader = candidate(
        **{
            FROZEN_DOC: edited,
            "requirements/frozen_hashes.json": json.dumps(
                {FROZEN_DOC: hashlib.sha256(edited.encode()).hexdigest()}
            ),
        }
    )
    errors = judge(trusted, reader, [FROZEN_DOC, "requirements/frozen_hashes.json"])
    assert any("differs from the digest recorded on main" in e for e in errors)
    assert any("frozen specification path modified" in e for e in errors)


def test_d_frozen_change_passes_with_a_real_owner_approval(tmp_path):
    """D, positive half: a genuine approval on main does authorise it."""
    edited = "# Canonical Master Specification\n\nOwner-approved edit.\n"
    trusted = make_trusted(
        tmp_path,
        approvals=[
            {
                "id": "spec-amendment",
                "milestone": "M1",
                "paths": [FROZEN_DOC, "requirements/frozen_hashes.json"],
                "reason": "owner amended the specification",
            }
        ],
    )
    reader = candidate(**{FROZEN_DOC: edited})
    assert judge(trusted, reader, [FROZEN_DOC, "requirements/frozen_hashes.json"]) == []


def test_e_neutering_the_local_checker_does_not_help(tmp_path):
    """E: the gate never imports candidate code, and the file is governed."""
    trusted = make_trusted(tmp_path)
    reader = candidate(**{"tools/check_scope.py": "def check(*a, **k):\n    return []\n"})
    errors = judge(trusted, reader, ["tools/check_scope.py", "dashboard/app.py"])
    assert any("tools/check_scope.py" in e and "owner approval" in e for e in errors)
    assert any("dashboard/app.py" in e for e in errors)


def test_f_modifying_the_verifier_itself_is_governed(tmp_path):
    """F: and under pull_request_target the candidate's copy never runs."""
    trusted = make_trusted(tmp_path)
    reader = candidate(**{"tools/governance/authoritative_check.py": "raise SystemExit(0)\n"})
    errors = judge(trusted, reader, ["tools/governance/authoritative_check.py"])
    assert any("owner approval" in e for e in errors)


def test_f_governance_workflow_is_governed(tmp_path):
    trusted = make_trusted(tmp_path)
    errors = judge(trusted, candidate(), [".github/workflows/governance.yml"])
    assert any("owner approval" in e for e in errors)


def test_g_a_candidate_supplied_key_confers_nothing(tmp_path):
    """G: there is no signature mechanism; a key file is an ordinary path."""
    trusted = make_trusted(tmp_path)
    reader = candidate(**{"configs/governance/allowed_signers": "ssh-ed25519 AAAA... agent\n"})
    errors = judge(trusted, reader, ["configs/governance/allowed_signers", "dashboard/app.py"])
    assert any("dashboard/app.py" in e for e in errors)
    assert any("configs/governance/allowed_signers" in e for e in errors)


def test_h_owner_approved_exceptional_path_passes(tmp_path):
    """H: the legitimate ceremony."""
    trusted = make_trusted(
        tmp_path,
        approvals=[
            {
                "id": "m1-gate",
                "milestone": "M1",
                "paths": ["scripts/ci_local.sh"],
                "reason": "the milestone gate is hardcoded and must move",
            }
        ],
    )
    assert judge(trusted, candidate(), ["scripts/ci_local.sh"]) == []


def test_h_approval_for_another_milestone_is_inert(tmp_path):
    """Permissions expire with the milestone that was granted them."""
    trusted = make_trusted(
        tmp_path,
        milestone="M1",
        approvals=[
            {
                "id": "m2-gate",
                "milestone": "M2",
                "paths": ["scripts/ci_local.sh"],
                "reason": "granted for a different milestone",
            }
        ],
    )
    errors = judge(trusted, candidate(), ["scripts/ci_local.sh"])
    assert any("outside the permitted scope of M1" in e for e in errors)


def test_i_normal_in_scope_work_passes_with_no_ceremony(tmp_path):
    """I: ordinary M2 work must stay frictionless."""
    trusted = make_trusted(tmp_path, milestone="M2")
    reader = candidate(milestone="M2")
    changed = [
        "python/futures/contracts.py",
        "cpp/replay/engine.cpp",
        "tests/unit/test_futures_contracts.py",
        "docs/FUTURES.md",
        "adr/0015-futures-contract-identity.md",
    ]
    assert judge(trusted, reader, changed) == []


@pytest.mark.parametrize(
    "policy_text, expected",
    [
        ("policy_version: 99\nactive_milestone: M1\n", "policy_version"),
        ("policy_version: 1\nactive_milestone: milestone one\n", "not a milestone ID"),
        ("just a string", "must be a mapping"),
        ("policy_version: 1\nactive_milestone: M1\napprovals: {}\n", "approvals must be a list"),
    ],
)
def test_j_malformed_policy_fails_closed(tmp_path, policy_text, expected):
    """J: absence or nonsense is never a pass."""
    trusted = make_trusted(tmp_path)
    (trusted / "configs/governance/policy.yaml").write_text(policy_text, encoding="utf-8")
    with pytest.raises(authoritative_check.PolicyError, match=expected):
        judge(trusted, candidate(), [])


def test_j_missing_policy_fails_closed(tmp_path):
    trusted = make_trusted(tmp_path)
    (trusted / "configs/governance/policy.yaml").unlink()
    with pytest.raises(authoritative_check.PolicyError, match="missing"):
        judge(trusted, candidate(), [])


def test_j_missing_build_state_fails_closed(tmp_path):
    trusted = make_trusted(tmp_path)
    errors = judge(trusted, MappingReader({FROZEN_DOC: FROZEN_TEXT}), [])
    assert any("missing from the candidate revision" in e for e in errors)


def test_j_deleted_frozen_file_is_detected(tmp_path):
    trusted = make_trusted(tmp_path)
    reader = MappingReader({"docs/BUILD_STATE.md": "- Active milestone: M1\n"})
    errors = judge(trusted, reader, [])
    assert any("frozen file is missing" in e for e in errors)


def test_j_approvals_may_not_use_globs(tmp_path):
    """A glob approval could widen far beyond what the owner reviewed."""
    trusted = make_trusted(
        tmp_path,
        approvals=[{"id": "broad", "milestone": "M1", "paths": ["scripts/**"], "reason": "no"}],
    )
    with pytest.raises(authoritative_check.PolicyError, match="glob"):
        judge(trusted, candidate(), ["scripts/ci_local.sh"])


# ------------------------------------------------------- committed fixture


def test_committed_counter_example_is_rejected(repo_root):
    """The negative gate CI runs, asserted here too so it fails fast locally."""
    fixture = repo_root / "tests/unit/fixtures/governance_tampered"
    changed = [
        line.strip()
        for line in (fixture / "changed_files.txt").read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    from readers import DirectoryReader

    errors = authoritative_check.run(
        fixture / "trusted", DirectoryReader(str(fixture / "candidate")), changed
    )
    assert errors, "the counter-example fixture must be rejected"

    joined = "\n".join(errors)
    # Each attempt must fail for its own reason, not collapse into one that
    # happens to fire — otherwise a regression in one route hides behind another.
    assert "trusted policy on main says 'M1'" in joined
    assert "configs/milestone_scope.yaml" in joined
    assert "differs from the digest recorded on main" in joined
    assert "dashboard/app.py" in joined


def test_live_repository_policy_is_loadable(repo_root):
    """The real policy on this branch must parse and declare one milestone."""
    policy = authoritative_check.load_policy(repo_root)
    assert policy["policy_version"] == 1
    assert policy["active_milestone"] in {f"M{n}" for n in range(10)}
    assert policy["governance_paths"], "an empty governance_paths list governs nothing"
