# Build State

- Active milestone: M0
- Active branch: `milestone/m0-foundation`
- Owner-approved scope changes: none
- Last audit: `/audit-milestone M0`, 2026-08-06 — 17 PASS, 5 FAIL; the five
  blocking findings are remediated (see `experiments/milestone-reports/M0.md` §13)
- Current blockers: none
- Deferred verification obligations: 10 open, listed in
  `docs/DEFERRED_VERIFICATION.md`; due M1=5, M2=2, M3=1, M4=1, M5=1

## M0 state

Implementation is complete and every stage of `scripts/ci_local.sh` passes. All
22 M0 requirements are `implemented`. **None is `verified`, and M0 is not closed.**
The audit confirmed twelve are admissible for promotion — AEGIS-001, 002, 003,
006, 007, 008, 010, 228, 231, 232, 235, 236 — but promoting them is an owner
decision that has not been taken.

See `experiments/milestone-reports/M0.md` for the full report and the audit record.

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
4. **Decide whether to promote the twelve audited requirements** to `verified`.
   The audit found them admissible; it deliberately did not perform the promotion.

Claude Code must update this file only when starting/closing a milestone. It must
not change the canonical specification.
