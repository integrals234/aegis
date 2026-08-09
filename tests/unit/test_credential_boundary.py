"""R8 Boundary 1 — the credential cannot reach the trust root (ADR-0014).

The content gate makes a candidate branch's files non-authoritative. That is
worth nothing on its own: the policy it trusts lives on ``main``, so if the
credential in the agent's environment can put a file on ``main``, the agent can
rewrite the rules it is judged by.

``tools/governance/verify_credential_boundary.py`` proves it cannot, against
live GitHub. These tests pin the *semantics* of that script over recorded API
shapes, with no network: if GitHub ever changes a status code or a field name,
this fails here rather than silently turning a probe into a no-op that always
passes.

The invariant under test is **not** "the agent cannot merge" — ``Contents:
Write`` is needed to push branches and is the same permission the merge endpoint
requires. It is:

    no commit enters `main` without a fresh approving review
    from the separate owner identity
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.unit

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "governance"))

import verify_credential_boundary as vcb  # noqa: E402

GOOD_PERMISSIONS = {
    "actions": "read",
    "administration": "read",
    "contents": "write",
    "metadata": "read",
    "pull_requests": "write",
    "workflows": "write",
}

RULESET = {
    "name": "Protect main",
    "enforcement": "active",
    "rules": [
        {"type": "deletion"},
        {"type": "non_fast_forward"},
        {
            "type": "pull_request",
            "parameters": {
                "required_approving_review_count": 1,
                "require_last_push_approval": True,
                "dismiss_stale_reviews_on_push": True,
            },
        },
        {
            "type": "required_status_checks",
            "parameters": {
                "strict_required_status_checks_policy": True,
                "required_status_checks": [{"context": f"check {n}"} for n in range(10)],
            },
        },
    ],
}


def fake_gh(responses):
    """Route gh() calls by a substring of their arguments."""

    def _gh(*args):
        joined = " ".join(args)
        for needle, (code, body) in responses.items():
            if needle in joined:
                return code, body
        raise AssertionError(f"unexpected gh call: {joined}")

    return _gh


def results(probes):
    return {p.name: p for p in probes}


# ------------------------------------------------------------------ status


@pytest.mark.parametrize(
    "text, expected",
    [
        ("gh: Not Found (HTTP 404)", "404"),
        ("HTTP 403: Resource not accessible by integration", "403"),
        ('{"ok": true}', "200"),
    ],
)
def test_http_status_extraction(text, expected):
    assert vcb.http_status(text) == expected


# --------------------------------------------------------------- V1, V2, V4


def test_v1_rejects_a_widened_grant(monkeypatch):
    """Administration: write is the escalation this probe exists to catch."""
    monkeypatch.setattr(vcb, "gh", fake_gh({"installation/repositories": (0, "integrals234/aegis\n")}))
    widened = dict(GOOD_PERMISSIONS, administration="write")
    probes = results(vcb.probe_identity({"permissions": widened, "expires_at": "2099-01-01T00:00:00Z"}))
    assert not probes["V1"].passed


def test_v1_rejects_an_extra_permission(monkeypatch):
    monkeypatch.setattr(vcb, "gh", fake_gh({"installation/repositories": (0, "integrals234/aegis\n")}))
    extra = dict(GOOD_PERMISSIONS, secrets="write")
    probes = results(vcb.probe_identity({"permissions": extra, "expires_at": "2099-01-01T00:00:00Z"}))
    assert not probes["V1"].passed, "an unreviewed extra permission must fail the probe"


def test_v2_rejects_a_multi_repository_installation(monkeypatch):
    monkeypatch.setattr(
        vcb, "gh", fake_gh({"installation/repositories": (0, "integrals234/aegis\nintegrals234/other\n")})
    )
    probes = results(vcb.probe_identity({"permissions": GOOD_PERMISSIONS, "expires_at": "2099-01-01T00:00:00Z"}))
    assert not probes["V2"].passed


def test_v4_rejects_a_long_lived_token(monkeypatch):
    monkeypatch.setattr(vcb, "gh", fake_gh({"installation/repositories": (0, "integrals234/aegis\n")}))
    probes = results(vcb.probe_identity({"permissions": GOOD_PERMISSIONS, "expires_at": "2099-01-01T00:00:00Z"}))
    assert not probes["V4"].passed, "a token valid for years is not an installation token"


# ------------------------------------------------------------ V6, V7, V8 (L)


def test_v6_403_is_a_pass_but_422_is_a_failure(monkeypatch):
    """The inversion that matters.

    GitHub authorises before it validates. A 403 means the ruleset write was
    denied. A 422 means it was *authorised* and only the payload was rejected —
    which would mean the agent could rewrite "Protect main" with a valid body.
    """
    denied = fake_gh(
        {
            "rulesets": (1, "gh: HTTP 403: Resource not accessible by integration"),
            "actions/permissions": (1, "gh: HTTP 403"),
            "branches/main/protection": (1, "gh: Not Found (HTTP 404)"),
        }
    )
    monkeypatch.setattr(vcb, "gh", denied)
    assert results(vcb.probe_denials())["V6"].passed

    authorised = fake_gh(
        {
            "rulesets": (1, "gh: HTTP 422: Validation Failed"),
            "actions/permissions": (1, "gh: HTTP 403"),
            "branches/main/protection": (1, "gh: Not Found (HTTP 404)"),
        }
    )
    monkeypatch.setattr(vcb, "gh", authorised)
    assert not results(vcb.probe_denials())["V6"].passed, (
        "422 means the write was permitted; that must fail the verification"
    )


def test_v7_actions_settings_write_denied(monkeypatch):
    monkeypatch.setattr(
        vcb,
        "gh",
        fake_gh(
            {
                "rulesets": (1, "gh: HTTP 403"),
                "actions/permissions": (0, '{"enabled": true}'),
                "branches/main/protection": (1, "gh: HTTP 404"),
            }
        ),
    )
    assert not results(vcb.probe_denials())["V7"].passed, (
        "a successful Actions-settings write means O4 can be undone by the agent"
    )


# ----------------------------------------------------------- V5 family (K, M)


def test_v5_family_reads_the_fresh_approval_requirement(monkeypatch):
    monkeypatch.setattr(
        vcb,
        "gh",
        fake_gh(
            {
                "rulesets/": (0, json.dumps(RULESET)),
                "rules/branches/main": (
                    0,
                    json.dumps([{"type": t} for t in
                                ("deletion", "non_fast_forward", "pull_request",
                                 "required_status_checks")]),
                ),
            }
        ),
    )
    probes = results(vcb.probe_ruleset())
    assert probes["V5"].passed
    assert probes["V5b"].passed, "one fresh approval must be required"
    assert probes["V5c"].passed, "the ten pre-existing checks must still be required"
    assert probes["V5d"].passed, "every rule must be enforced against this token"


@pytest.mark.parametrize(
    "weakened",
    [
        {"required_approving_review_count": 0},
        {"require_last_push_approval": False},
        {"dismiss_stale_reviews_on_push": False},
    ],
)
def test_v5b_rejects_each_weakening(monkeypatch, weakened):
    """Each of the three settings independently carries attack K or K'."""
    ruleset = json.loads(json.dumps(RULESET))
    for rule in ruleset["rules"]:
        if rule["type"] == "pull_request":
            rule["parameters"].update(weakened)
    monkeypatch.setattr(
        vcb,
        "gh",
        fake_gh({"rulesets/": (0, json.dumps(ruleset)), "rules/branches/main": (0, "[]")}),
    )
    assert not results(vcb.probe_ruleset())["V5b"].passed


def test_v5c_rejects_dropped_required_checks(monkeypatch):
    ruleset = json.loads(json.dumps(RULESET))
    for rule in ruleset["rules"]:
        if rule["type"] == "required_status_checks":
            rule["parameters"]["required_status_checks"] = [{"context": "only one"}]
    monkeypatch.setattr(
        vcb,
        "gh",
        fake_gh({"rulesets/": (0, json.dumps(ruleset)), "rules/branches/main": (0, "[]")}),
    )
    assert not results(vcb.probe_ruleset())["V5c"].passed


def test_v5d_detects_a_bypassing_actor(monkeypatch):
    """`bypass_actors` is invisible to the App, but its effect is not.

    /rules/branches/main is evaluated for the calling actor, so a token that
    could bypass would see the rules omitted. That is the App-specific half of
    the check the owner performs by eye at O5 and O6.
    """
    monkeypatch.setattr(
        vcb,
        "gh",
        fake_gh({"rulesets/": (0, json.dumps(RULESET)), "rules/branches/main": (0, "[]")}),
    )
    assert not results(vcb.probe_ruleset())["V5d"].passed


# ----------------------------------------------------------------- V11 (N)


@pytest.mark.parametrize(
    "settings, expected",
    [
        ({"default_workflow_permissions": "read", "can_approve_pull_request_reviews": False}, True),
        ({"default_workflow_permissions": "write", "can_approve_pull_request_reviews": False}, False),
        ({"default_workflow_permissions": "read", "can_approve_pull_request_reviews": True}, False),
    ],
)
def test_v11_actions_hardening(monkeypatch, settings, expected):
    """Load-bearing: the agent holds Workflows: Write, so if either of these
    regresses, a candidate workflow could use GITHUB_TOKEN to approve itself."""
    monkeypatch.setattr(
        vcb, "gh", fake_gh({"actions/permissions/workflow": (0, json.dumps(settings))})
    )
    assert results(vcb.probe_actions_hardening())["V11"].passed is expected


# ------------------------------------------------------------- V3, V9, V10 (K)


def test_v3_rejects_the_owner_identity(monkeypatch):
    monkeypatch.setattr(
        vcb,
        "gh",
        fake_gh(
            {
                "pulls/7/reviews": (1, "gh: HTTP 422"),
                "pulls/7": (0, json.dumps({"user": {"login": "integrals234"}, "mergeable_state": "blocked"})),
            }
        ),
    )
    assert not results(vcb.probe_pull_request(7))["V3"].passed


def test_v9_v10_unapproved_pr_is_blocked_and_unapprovable(monkeypatch):
    monkeypatch.setattr(
        vcb,
        "gh",
        fake_gh(
            {
                "pulls/7/reviews": (1, "gh: HTTP 422: Can not approve your own pull request"),
                "pulls/7": (
                    0,
                    json.dumps(
                        {"user": {"login": "aegis-agent-integrals234[bot]"},
                         "mergeable_state": "blocked"}
                    ),
                ),
            }
        ),
    )
    probes = results(vcb.probe_pull_request(7))
    assert probes["V3"].passed
    assert probes["V9"].passed
    assert probes["V10"].passed


def test_v10_fails_if_self_approval_ever_succeeds(monkeypatch):
    """If GitHub ever permits self-approval, attack K reopens."""
    monkeypatch.setattr(
        vcb,
        "gh",
        fake_gh(
            {
                "pulls/7/reviews": (0, '{"id": 1, "state": "APPROVED"}'),
                "pulls/7": (
                    0,
                    json.dumps({"user": {"login": "aegis-agent-integrals234[bot]"},
                                "mergeable_state": "blocked"}),
                ),
            }
        ),
    )
    assert not results(vcb.probe_pull_request(7))["V10"].passed


def test_v9_fails_when_a_pr_is_mergeable_without_review(monkeypatch):
    monkeypatch.setattr(
        vcb,
        "gh",
        fake_gh(
            {
                "pulls/7/reviews": (1, "gh: HTTP 422"),
                "pulls/7": (
                    0,
                    json.dumps({"user": {"login": "aegis-agent-integrals234[bot]"},
                                "mergeable_state": "clean"}),
                ),
            }
        ),
    )
    assert not results(vcb.probe_pull_request(7))["V9"].passed
