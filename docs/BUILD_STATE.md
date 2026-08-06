# Build State

- Active milestone: M0
- Active branch: `milestone/m0-foundation`
- Owner-approved scope changes: none
- Last audit: not run — `/audit-milestone M0` is the requested next step
- Current blockers: none
- Deferred verification obligations: 10 open, listed in `docs/DEFERRED_VERIFICATION.md`

## M0 state

Implementation is complete and every stage of `scripts/ci_local.sh` passes. All
22 M0 requirements are `implemented`; none is `verified`, because promotion
requires an independent spec-auditor review that has not run.

See `experiments/milestone-reports/M0.md` for the full report.

## Owner actions outstanding

These are not milestone blockers, but four requirements carry registered
obligations until they happen:

1. **Create the git remote** and push `milestone/m0-foundation` so
   `.github/workflows/ci.yml` executes for the first time (AEGIS-227, AEGIS-233,
   AEGIS-234).
2. **Protect the default branch** to require a passing workflow — this is the
   literal wording of AEGIS-234's acceptance criterion.
3. **Enable Docker Desktop WSL integration**, or run CI, so a clean-machine
   environment transcript exists (AEGIS-009).

Claude Code must update this file only when starting/closing a milestone. It must
not change the canonical specification.
