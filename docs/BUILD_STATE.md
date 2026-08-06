# Build State

- Active milestone: M0
- M0 status: **CLOSED** 2026-08-06 (owner-approved fast-track closure)
- Active branch: `milestone/m0-foundation`
- Owner-approved scope changes: none
- Last audit: `/audit-milestone M0`, 2026-08-06, against `adf34ed` — 17 PASS, 5
  FAIL; the five blocking findings were remediated in R1–R4
  (see `experiments/milestone-reports/M0.md` §13). Closure was performed
  fast-track on the owner's instruction using that audit plus the completed
  remediation; **no second independent audit was run.**
- Current blockers: none
- Deferred verification obligations: 10 open, listed in
  `docs/DEFERRED_VERIFICATION.md`; due M1=5, M2=2, M3=1, M4=1, M5=1
- Next milestone: M1, **not started**. Starting it is a separate session.

## M0 state

Implementation is complete and every stage of `scripts/ci_local.sh` passes (23
stages, post-remediation). Of the 22 M0 requirements, **12 are `verified` and 10
remain `implemented`** with their verification obligations registered.

Verified: AEGIS-001, 002, 003, 006, 007, 008, 010, 228, 231, 232, 235, 236.

Still `implemented` with an open obligation: AEGIS-004, 005, 009, 227, 229, 230,
233, 234, 237, 238. Each one's frozen acceptance criterion names something that
does not exist yet; `tools/audit_requirements.py` refuses to promote them while
the obligation is open.

See `experiments/milestone-reports/M0.md` for the full report and the audit record.

## Owner actions outstanding

These are not M0 blockers — M0 is closed — but they are the events that
discharge four of the ten registered obligations:

1. **Create the git remote** and push `milestone/m0-foundation` so
   `.github/workflows/ci.yml` executes for the first time (AEGIS-227, AEGIS-233,
   AEGIS-234).
2. **Protect the default branch** to require a passing workflow — this is the
   literal wording of AEGIS-234's acceptance criterion.
3. **Enable Docker Desktop WSL integration**, or run CI, so a clean-machine
   environment transcript exists (AEGIS-009).

Claude Code must update this file only when starting/closing a milestone. It must
not change the canonical specification.
