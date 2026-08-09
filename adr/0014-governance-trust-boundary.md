# ADR-0014: Governance trust boundary

- Status: Accepted
- Date: 2026-08-09
- Requirement IDs: AEGIS-001, AEGIS-007, AEGIS-008
- Milestone: M2

## Context

The independent M1 audit recorded one open governance item, R8:
`tools/check_scope.py` derived owner scope approvals from `docs/BUILD_STATE.md`,
and `configs/milestone_scope.yaml` lists that file under `always_allowed`. The
agent could therefore authorise its own out-of-scope modification by writing the
file it is explicitly permitted to write.

Designing the fix surfaced three more instances of the same defect, none of them
in the original report:

1. **The frozen-file gate shares the channel.** `tools/check_frozen.py` parses
   the *same* `- Owner-approved scope changes:` line, and its `protected` set is
   `manifest ∪ {requirements/frozen_hashes.json}`. Naming both
   `docs/MASTER_SPEC.md` and the manifest on that line, editing the
   specification and recomputing its digest passed **both** the content and the
   history check. The `PreToolUse` hook in `.claude/hooks/protect_spec.py` does
   not close it — that hook only sees writes made through Claude's file tools,
   as its own docstring records, so a shell write bypasses it.
2. **The scope policy is agent-writable.** `configs/milestone_scope.yaml` lives
   in `configs/**`, which every milestone allows, so a branch could widen its
   own `allowed` list.
3. **The active-milestone line is an authority.** `- Active milestone: M9`
   selects a milestone whose `denied` list is empty and whose `allowed` list
   covers the tree.

Two candidate fixes were designed and discarded before the accepted one.

The first put an owner **signature** in the repository: an anchor file listing
approvals, signed with `ssh-keygen -Y sign`, verified against a committed
`allowed_signers`, with the signer file's own digest pinned inside the signed
anchor. This is circular. Every link of the chain is inside the branch under
judgement, so an agent generates its own keypair, replaces `allowed_signers`,
rewrites the anchor to record the new digest and arbitrary approvals, and signs
the result with its own key. Verification succeeds, because signature, signer
file and payload are mutually consistent. A hash chain confined to the
attacker's writable set proves internal consistency, not authenticity.

The second moved the root of trust to protected `main` and asserted that only
the owner could merge. Measurement showed that assumption was false: the
credential in the agent's environment was the repository owner's classic token,
`{"admin": true}`, scopes `repo, workflow`. It could merge any pull request, and
it could `PUT` the "Protect main" ruleset — so `bypass_actors: []` protected
nothing against an actor able to rewrite the ruleset first.

The governing assumption is therefore: **the agent can write every file in the
branch under judgement and holds whatever GitHub credential is in its
environment.** A control that lives in the branch, or that depends on the agent
choosing not to use a capability it has, is not a control.

## Decision

Two boundaries, neither sufficient alone.

**Content — what the branch says is evidence, never authority.**
`configs/governance/policy.yaml` on protected `main` is the sole source of the
active milestone, the governance-path list and owner approvals.
`.github/workflows/governance.yml` runs `tools/governance/authoritative_check.py`
under **`pull_request_target`**, so both the workflow definition and the checker
come from the base branch. The event choice is load-bearing: under
`pull_request` the workflow file comes from the head, so a candidate could keep
the job name — and with it the required status check — while replacing the body
with `exit 0`.

The privileged job never executes candidate code. It does not check out the
head, source candidate scripts, run candidate Python or binaries, or install
candidate-controlled dependencies; the lockfile it installs is `main`'s. The
candidate is read through the GitHub API as bytes, so its tree is never placed
on the runner's disk. Six checks, all fail-closed, are listed in the module
docstring.

Approvals name **exact paths, never globs**, and carry the milestone they were
granted for; an approval for another milestone is inert, so permissions expire
with the milestone. `configs/governance/policy.yaml` is itself exempt from
requiring approval, because it is inert until merged and merging is the owner's
act — which keeps the ceremony to one small reviewable pull request.

**Credential — the trust root is unreachable from the agent's environment.**
The agent authenticates as a private **GitHub App** installed only on this
repository, using installation access tokens that expire hourly, granted
`Contents: Write`, `Pull requests: Write`, `Workflows: Write`, `Actions: Read`,
`Metadata: Read` and `Administration: **Read**`. Withholding
`Administration: Write` is what denies ruleset and Actions-settings writes.

A GitHub App rather than a machine account with a fine-grained token: a
fine-grained personal access token cannot be used to contribute to a repository
whose owner is an outside collaborator on it, which is exactly a collaborator on
a personal-account repository.

**The invariant is not "the agent cannot merge."** `Contents: Write` is required
to push branches and is the same permission the merge endpoint needs; the two
cannot be separated by permission. The invariant is:

> No commit enters `main` without a fresh approving review from the separate
> owner identity.

enforced at the ruleset layer — `required_approving_review_count: 1`,
`require_last_push_approval: true`, `dismiss_stale_reviews_on_push: true`,
`bypass_actors: []` — together with GitHub refusing to let a pull request's
author approve it. After the owner approves an exact diff the agent could call
merge on those same bytes; that is not an escalation, because any deviation
voids the approval. The agent stopping before merge is a **workflow convention**
and is deliberately excluded from the proof.

`tools/check_scope.py` and `tools/check_frozen.py` are retained unchanged in
logic and **retitled advisory**. Their `- Owner-approved scope changes:` line
becomes a historical record that grants nothing.

## Alternatives considered

- **In-branch signed anchor** — circular, as shown above.
- **Owner-signed git commits** — same key management, and rebase or squash
  destroys the signatures the gate would depend on.
- **A `pull_request` workflow that fetches main's checker** — the workflow file
  still comes from the head, so the job can be neutered while keeping its name.
- **GitHub labels or PR reviews as the approval channel** — mutable by any
  write-capable token, including the agent's.
- **A single identity with required reviews** — deadlocks: GitHub refuses
  self-approval, and with one identity nobody else can approve. This is why two
  identities are structural rather than stylistic.
- **Separating merge from push by permission** — impossible; both require
  `Contents: Write`. Hence the ruleset-layer fix.
- **A prompt-level rule that Claude must not merge** — a policy, not a control.
  Retained as a convention, excluded from the proof.
- **Granting `Administration: Write` so the gate could read `bypass_actors`** —
  rejected outright. That field's visibility is not worth the capability;
  `GET /repos/{owner}/{repo}/rules/branches/main` shows which rules are enforced
  against the calling token, which is the App-specific half, and the owner
  verifies the field itself.

## Consequences

- One small owner-merged pull request per milestone transition and per genuine
  exception. Ordinary in-scope work needs no ceremony.
- Local feedback is preserved: `scripts/governance_preflight.sh` materialises
  `origin/main` in a throwaway worktree and runs **main's** checker against the
  working branch, so the authoritative verdict is available before a pull
  request exists.
- Limits, recorded rather than hidden:
  1. The bootstrap pull request that introduces this mechanism is trusted by
     owner review alone. That is irreducible — it is the trust root — and is why
     it carries mechanism only, and no futures or replay code.
  2. `Workflows: Write` is the widest permission the agent holds. Its only
     mitigations are the repository Actions settings (`GITHUB_TOKEN` read-only,
     Actions may not create or approve pull requests) and `.github/workflows/**`
     being a governance path. Those settings are therefore **load-bearing**, and
     are re-verified by probe V11 at every milestone close rather than assumed.
  3. The gate governs scope, frozen-file and milestone authority. It does not
     prove that tests were not gutted; that remains owner review plus the other
     required checks.
  4. It depends on documented GitHub semantics: `pull_request_target` uses the
     base-branch workflow definition and base-ref checkout, self-approval is
     refused, and ruleset writes require repository admin.
  5. The credential boundary is environment-dependent. Restoring an
     owner-credentialed `gh` or git configuration to the agent's machine removes
     it. This is **detected, not prevented** — probes V1–V4 re-run at each
     milestone close and fail if the identity is not the App's.
  6. The App private key sits on the same machine as the agent, so possession is
     no barrier. Acceptable because a token minted from it carries exactly the
     App's permission set: the boundary is the permission grant, not key secrecy
     from the agent. The key lives outside the repository so it cannot be
     committed, and rotation is an owner action.

## Verification

- `tests/unit/test_authoritative_governance.py` — attacks A–J, each building a
  trusted tree and a separate candidate tree, with no keys, no network and no
  GitHub. Includes the positive cases: an owner-approved exceptional path
  passes, and ordinary in-scope work passes with no ceremony.
- `tests/unit/fixtures/governance_tampered/` — a committed counter-example
  candidate attempting every self-authorisation route at once. Run as a negative
  gate in `.github/workflows/ci.yml`, in `scripts/ci_local.sh` and in the
  governance workflow itself; each attempt is asserted to fail for its own
  distinct reason rather than collapsing into one error that happens to fire.
- `tests/unit/test_credential_boundary.py` — pins the semantics of the live
  battery over recorded API shapes, including the inversion that a `422` from
  the ruleset write probe is a **failure** (it would mean the write was
  authorised and only the payload rejected).
- `tools/governance/verify_credential_boundary.py` — the live battery, run
  before the bootstrap pull request opens, at its merge, and at every milestone
  close. Every probe's expected result is a rejection; write endpoints are
  called only with structurally invalid payloads; **the merge endpoint is never
  called**, because under the corrected invariant a post-approval merge would
  succeed and so is not a safe probe.

## Owner approval

The R8 remediation was authorised by the owner in the M2 planning session on
2026-08-09, across four plan revisions in which the owner identified both the
circularity of the signed-anchor design and the credential gap that followed it.
The owner performed the setup this ADR depends on — App registration and
installation, key generation, the environment credential swap, and the ruleset
change — and ratifies the mechanism by approving the pull request that
introduces it.
