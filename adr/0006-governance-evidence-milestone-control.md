# ADR-0006: Governance, evidence rules and milestone control

- Status: Accepted
- Date: 2026-08-06
- Requirement IDs: AEGIS-001, AEGIS-002, AEGIS-003, AEGIS-007, AEGIS-233, AEGIS-234
- Milestone: M0

## Context

AEGIS is built by an AI agent under a frozen specification. The failure modes
that matter are not compile errors; they are a requirement marked complete
because code exists, a specification edited to match an implementation, a later
milestone's work drifting into an earlier one, and a claim that outlives the
evidence it was based on.

The scaffold's original controls were structurally weak in four ways, each
verified before this decision was written: the specification hook failed open on
malformed input and left its own tamper detector writable; any path that existed
counted as evidence, including a directory or an empty file; the status writer
could record `verified` with no evidence at all; and the audit tool's paths were
module constants, so it could not be tested against anything but the live tree.

## Decision

**Frozen-file protection, in order of authority.**

1. SHA-256 manifest checked by `tools/audit_requirements.py` on every run.
2. `tools/check_frozen.py`, which additionally fails if a frozen path appears in
   the branch diff — so editing a file and re-freezing its hash does not pass.
3. Pre-commit hook and CI running both.
4. Owner approval recorded in `docs/BUILD_STATE.md`, the only legitimate route
   to a specification change.

The PreToolUse hook is **defence in depth, not the control of record**. It sees
only writes made through Claude's file tools; anything routed through a shell
bypasses it entirely. It is hardened to fail closed on unparseable input, a
non-object payload and a missing project directory, and extended to cover
`requirements/frozen_hashes.json` — guarding the specification while leaving its
tamper detector writable protects the document from editing but not from having
its detector rewritten. `docs/LIMITATIONS.md` states plainly what it cannot see.

**Evidence rules.** A path that exists is not evidence. A directory, an empty
file, a `.gitkeep`, an `__init__.py` and a file containing only TODOs are each
rejected. `verified` additionally requires an `audit` record naming the reviewer,
the commit reviewed and the date. `tools/update_status.py` validates before
writing, using the same predicates the auditor uses, so an unsupportable claim
cannot be recorded in the first place and the writer cannot drift from the
checker.

**Deferred verification.** A requirement that is honestly `implemented` but
whose frozen acceptance names something that does not exist yet carries
`verification_blocked_until` plus a `residual`. The auditor **hard-fails** on
`verified` while an obligation is open, which converts the anti-inflation rule
from a convention into a gate, and `--check-deferred M<n>` fails the milestone
that owes the evidence if it closes without paying. This deliberately overrides
the close-milestone skill's default instruction to promote passing requirements
to `verified`.

**Scope control.** `docs/BUILD_STATE.md` names exactly one active milestone.
`configs/milestone_scope.yaml` declares the paths each milestone may touch and
which later-milestone paths are explicitly denied, and `tools/check_scope.py`
fails on the branch diff. Owner-approved exceptions are read from BUILD_STATE.

**Test layers.** Five pytest markers and matching ctest labels, each run and
reported separately, and **a layer that collects zero tests fails**. A green
empty layer is a stub presented as completion.

**Every gate ships with a counter-example** it must reject, and CI fails if any
gate starts accepting its counter-example. A gate that cannot fail proves
nothing.

## Alternatives considered

**Trusting the PreToolUse hook as the primary control.** Rejected: it cannot see
shell-mediated writes, and treating a partial control as complete is worse than
having no control, because it stops anyone looking further.

**Intercepting Bash writes to frozen files.** Rejected: the interception surface
is unbounded (redirects, `sed -i`, `python -c`, editors), so the effort buys
partial coverage while implying full coverage.

**Marking blocked requirements `blocked` rather than `implemented`.** Rejected:
`docs/ROADMAP.md` requires listed IDs to be at least `implemented` for a
milestone to close, so `blocked` would prevent M0 closing while describing work
that is genuinely done. The registered obligation records the same fact without
the false implication that nothing was built.

**Marking them `deferred`.** Rejected and impossible: all are `priority: must`,
and the auditor already errors on that combination.

## Consequences

- `verified` is expensive: it needs evidence that says something and a named
  independent review. That is the intent.
- Ten M0 requirements close `implemented` with obligations rather than
  `verified`; `docs/DEFERRED_VERIFICATION.md` lists them and CI drift-checks it.
- Adding a source directory requires a scope-file edit.
- The pre-commit hook can be skipped locally; CI runs the same checks
  independently, so a hook nobody installed is never the only gate.

## Verification

- `tests/unit/test_audit_requirements.py`, `test_evidence_rules.py`,
  `test_frozen_integrity.py`, `test_protect_spec_hook.py`, `test_scope_guard.py`,
  `test_ci_parity.py`.
- Frozen-file mutation is tested against a scratch copy of the tree; a test that
  proves tamper detection by tampering with the artifact it protects is not one
  worth having.
- `scripts/ci_local.sh` and `.github/workflows/ci.yml`, including the
  `negative-gates` job.

## Owner approval

Recorded in the approved M0 plan (`experiments/plans/M0.md`, Part 4 and Part 9).
