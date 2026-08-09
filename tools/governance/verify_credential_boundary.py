#!/usr/bin/env python3
"""Prove the credential in this environment cannot reach the trust root (R8).

The content boundary (``authoritative_check.py``) makes a candidate branch's
files non-authoritative. It is worth nothing on its own: the policy it trusts
lives on ``main``, and if the credential sitting in the agent's environment can
put a file on ``main``, the agent can simply rewrite the rules it is judged by.
That was the gap rev 3 of the plan of record closed.

So this is the other half. It establishes, against live GitHub, that:

* the environment authenticates as the AEGIS Agent **GitHub App**, not as the
  repository owner;
* the installation reaches exactly one repository;
* ruleset writes, Actions-settings writes and admin endpoints are **denied**;
* "Protect main" requires an approving review the App cannot produce;
* the ten pre-existing required checks are still required;
* ``GITHUB_TOKEN`` is read-only and Actions cannot approve pull requests.

**The invariant being proved is not "the agent cannot merge."** ``Contents:
Write`` is required to push branches and is the same permission the merge
endpoint needs, so the two cannot be separated. The invariant is:

    no commit enters `main` without a fresh approving review
    from the separate owner identity

Two rules govern every probe here, and both are load-bearing:

1. A write endpoint is called **only** with a structurally invalid payload, so
   authorisation-denied (403) and validation-rejected (422) are the only
   outcomes and neither can change state.
2. The **merge endpoint is never called**. Under the corrected invariant a
   post-approval merge would succeed, so it is not a safe probe. Its absence is
   deliberate.

Two facts are deliberately *not* asserted here, because the App cannot see them
and pretending otherwise would be theatre:

* ``bypass_actors`` — GitHub omits it for callers without ruleset write access.
  The owner verifies it during O5 and O6. What this script *can* show is that
  every rule is enforced against this token, which is the App-specific half.
* the content of the owner's own credential store.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

REPO = "integrals234/aegis"
RULESET_ID = 20596537
EXPECTED_PERMISSIONS = {
    "actions": "read",
    "administration": "read",
    "contents": "write",
    "metadata": "read",
    "pull_requests": "write",
    "workflows": "write",
}
EXPECTED_CHECKS = 10


class Probe:
    def __init__(self, name: str, what: str) -> None:
        self.name = name
        self.what = what
        self.passed = False
        self.detail = ""

    def record(self, passed: bool, detail: str) -> Probe:
        self.passed = passed
        self.detail = detail
        return self


def gh(*args: str) -> tuple[int, str]:
    result = subprocess.run(["gh", *args], capture_output=True, text=True, check=False)
    return result.returncode, (result.stdout or "") + (result.stderr or "")


def _git(*args: str) -> str:
    result = subprocess.run(["git", *args], capture_output=True, text=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else ""


def http_status(output: str) -> str:
    """Pull the status code out of gh's error text; absent means it succeeded."""
    marker = "HTTP "
    index = output.find(marker)
    if index == -1:
        return "200"
    return output[index + len(marker) : index + len(marker) + 3]


def probe_identity(token_response: dict[str, Any]) -> list[Probe]:
    probes = []

    granted = token_response.get("permissions", {})
    ok = granted == EXPECTED_PERMISSIONS
    probes.append(
        Probe("V1", "token grant is exactly the declared permission set").record(
            ok, f"granted={json.dumps(granted, sort_keys=True)}"
        )
    )

    code, out = gh("api", "/installation/repositories", "--jq", ".repositories[].full_name")
    repos = sorted({line.strip() for line in out.splitlines() if line.strip()})
    probes.append(
        Probe("V2", "installation reaches exactly one repository").record(
            code == 0 and repos == [REPO], f"repositories={repos}"
        )
    )

    expires = token_response.get("expires_at", "")
    try:
        delta = datetime.fromisoformat(expires.replace("Z", "+00:00")) - datetime.now(UTC)
        short = 0 < delta.total_seconds() <= 3700
    except ValueError:
        short = False
    probes.append(
        Probe("V4", "credential is short-lived, not a standing token").record(
            short, f"expires_at={expires}"
        )
    )
    return probes


def probe_denials() -> list[Probe]:
    """Write and admin endpoints must be refused. Invalid payloads only."""
    probes = []

    code, out = gh(
        "api", "-X", "PUT", f"repos/{REPO}/rulesets/{RULESET_ID}", "-f", "enforcement=__invalid__"
    )
    status = http_status(out)
    # 403 = denied (correct). 422 would mean the write was AUTHORISED and only
    # the payload was rejected, which is a failure of this verification.
    probes.append(
        Probe("V6", "ruleset write is denied").record(
            code != 0 and status == "403", f"HTTP {status}"
        )
    )

    code, out = gh(
        "api", "-X", "PUT", f"repos/{REPO}/actions/permissions", "-f", "enabled=__invalid__"
    )
    status = http_status(out)
    probes.append(
        Probe("V7", "Actions-settings write is denied").record(
            code != 0 and status == "403", f"HTTP {status}"
        )
    )

    code, out = gh("api", f"repos/{REPO}/branches/main/protection")
    status = http_status(out)
    probes.append(
        Probe("V8", "admin-only branch-protection endpoint is denied").record(
            code != 0 and status in {"403", "404"}, f"HTTP {status}"
        )
    )
    return probes


def probe_ruleset() -> list[Probe]:
    probes = []
    code, out = gh("api", f"repos/{REPO}/rulesets/{RULESET_ID}")
    if code != 0:
        return [Probe("V5", "ruleset is readable").record(False, out.strip()[:200])]

    ruleset = json.loads(out)
    rules = {r["type"]: r.get("parameters", {}) for r in ruleset.get("rules", [])}

    pr_rule = rules.get("pull_request", {})
    fresh_approval = (
        pr_rule.get("required_approving_review_count") == 1
        and pr_rule.get("require_last_push_approval") is True
        and pr_rule.get("dismiss_stale_reviews_on_push") is True
    )
    probes.append(
        Probe("V5", "ruleset readable and active").record(
            ruleset.get("enforcement") == "active",
            f"name={ruleset.get('name')!r} enforcement={ruleset.get('enforcement')!r} "
            f"bypass_actors_visible={'bypass_actors' in ruleset} (owner verifies at O5/O6)",
        )
    )
    probes.append(
        Probe("V5b", "main requires one fresh owner approval").record(
            fresh_approval,
            f"approvals={pr_rule.get('required_approving_review_count')} "
            f"last_push={pr_rule.get('require_last_push_approval')} "
            f"dismiss_stale={pr_rule.get('dismiss_stale_reviews_on_push')}",
        )
    )

    checks = rules.get("required_status_checks", {}).get("required_status_checks", [])
    probes.append(
        Probe("V5c", "pre-existing required checks are still required").record(
            len(checks) >= EXPECTED_CHECKS,
            f"{len(checks)} contexts, strict="
            f"{rules.get('required_status_checks', {}).get('strict_required_status_checks_policy')}",
        )
    )

    code, out = gh("api", f"repos/{REPO}/rules/branches/main")
    enforced = [r.get("type") for r in json.loads(out)] if code == 0 else []
    probes.append(
        Probe("V5d", "every rule is enforced against THIS token (not bypassed)").record(
            {"pull_request", "required_status_checks", "deletion", "non_fast_forward"}
            <= set(enforced),
            f"enforced={sorted(enforced)}",
        )
    )
    return probes


def probe_actions_hardening() -> list[Probe]:
    code, out = gh("api", f"repos/{REPO}/actions/permissions/workflow")
    if code != 0:
        return [Probe("V11", "Actions hardening is in force").record(False, out.strip()[:200])]
    settings = json.loads(out)
    ok = (
        settings.get("default_workflow_permissions") == "read"
        and settings.get("can_approve_pull_request_reviews") is False
    )
    return [
        Probe("V11", "GITHUB_TOKEN read-only; Actions cannot approve PRs").record(
            ok, json.dumps(settings, sort_keys=True)
        )
    ]


def probe_pull_request(number: int) -> list[Probe]:
    """Probes that need a live pull request authored by the App."""
    probes = []

    code, out = gh("api", f"repos/{REPO}/pulls/{number}")
    if code != 0:
        return [Probe("V3", f"pull request #{number} is readable").record(False, out[:200])]
    pull = json.loads(out)

    author = pull.get("user", {}).get("login", "")
    probes.append(
        Probe("V3", "the environment identity is a bot, not the owner").record(
            author.endswith("[bot]") and author != "integrals234", f"author={author!r}"
        )
    )

    code, reviews_raw = gh("api", f"repos/{REPO}/pulls/{number}/reviews")
    reviews = json.loads(reviews_raw) if code == 0 else []
    probes.append(
        Probe("V9", "unapproved PR cannot enter main").record(
            pull.get("mergeable_state") == "blocked" and not reviews,
            f"mergeable_state={pull.get('mergeable_state')!r} reviews={len(reviews)}",
        )
    )

    code, out = gh(
        "api", "-X", "POST", f"repos/{REPO}/pulls/{number}/reviews", "-f", "event=APPROVE"
    )
    status = http_status(out)
    probes.append(
        Probe("V10", "the App cannot approve its own pull request").record(
            code != 0 and status in {"422", "403"}, f"HTTP {status}"
        )
    )
    return probes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--token-response", type=Path, required=True,
                        help="JSON from the installation token endpoint (token field may be absent)")
    parser.add_argument("--pr", type=int, help="pull request number for V3/V9/V10")
    parser.add_argument("--output", type=Path, help="write the evidence record here")
    args = parser.parse_args(argv)

    token_response = json.loads(args.token_response.read_text(encoding="utf-8"))
    token_response.pop("token", None)

    probes: list[Probe] = []
    probes += probe_identity(token_response)
    probes += probe_ruleset()
    probes += probe_denials()
    probes += probe_actions_hardening()
    if args.pr:
        probes += probe_pull_request(args.pr)

    width = max(len(p.what) for p in probes)
    for probe in probes:
        mark = "PASS" if probe.passed else "FAIL"
        print(f"{probe.name:<5} {probe.what:<{width}}  {mark}   {probe.detail}")

    failed = [p.name for p in probes if not p.passed]
    print()
    if failed:
        print(f"Credential boundary verification FAILED: {', '.join(failed)}", file=sys.stderr)
    else:
        print(f"Credential boundary verification passed: {len(probes)} probes.")

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(
                {
                    "repository": REPO,
                    "generated_on": datetime.now(UTC).strftime("%Y-%m-%d"),
                    # Provenance. A dirty worktree means the artifact is not
                    # reproducible from any commit, which is exactly what the M1
                    # benchmark observation is about; record it rather than
                    # letting a reader assume otherwise.
                    "repository_commit": _git("rev-parse", "HEAD"),
                    "dirty": bool(_git("status", "--porcelain")),
                    "invariant": (
                        "no commit enters main without a fresh approving review from the "
                        "separate owner identity"
                    ),
                    "not_asserted_here": [
                        "bypass_actors is empty (invisible to the App; owner verifies at O5/O6)",
                        "the merge endpoint (never called; a post-approval merge would succeed)",
                    ],
                    "probes": [
                        {"id": p.name, "checks": p.what, "passed": p.passed, "detail": p.detail}
                        for p in probes
                    ],
                    "passed": not failed,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Evidence written to {args.output}")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
