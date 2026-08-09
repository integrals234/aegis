# Build State

- Active milestone: M1
- M0 status: **CLOSED** 2026-08-06 (owner-approved fast-track closure)
- M1 status: **IMPLEMENTATION COMPLETE, AWAITING INDEPENDENT `/audit-milestone M1`**,
  started 2026-08-07 per `experiments/plans/M1.md` (approved plan of record,
  revised after nine owner corrections). Slices 1-11 committed; slice 12
  (closure gate) not yet run.
- Active branch: `milestone/m1-exchange-core`
- Owner-approved scope changes: `scripts/ci_local.sh`, `.github/workflows/ci.yml`
  — retargeting the hardcoded `--milestone M0` gate to M1
  (`experiments/plans/M1.md` §2 gap 3, §5 slice 0). Neither path is in M1's
  `allowed` list in `configs/milestone_scope.yaml` (only M0 and M9 list
  `scripts/**`/`.github/**`), but the milestone gate itself cannot move
  without editing them, and the plan of record calls for this exact edit.
- Last audit: independent `/audit-milestone M1`, 2026-08-08, against `b33ca67` —
  all 15 M1 requirements PASS, no blocking finding; eight remediation items
  R1–R8 were raised, R1–R7 applied in `82aaeee` and R8 (the owner-approval
  channel living in this agent-writable file) deliberately left open. The
  earlier `/audit-milestone M0`, 2026-08-06 against `adf34ed`, is recorded in
  `experiments/milestone-reports/M0.md` §13. **No M1 requirement has been
  promoted to `verified`**: promotion additionally requires an audit record
  (`--auditor/--audit-commit/--audit-date`) against the final tree, which has
  not been written.
- CI is live. PR #1 merged to `main` as `ddc82f8`; the push-triggered run
  [31286449399](https://github.com/integrals234/aegis/actions/runs/31286449399)
  passed all nine jobs on `main`. `main` is protected by repository ruleset
  "Protect main" (id 20596537, active): pull request required, and all nine
  AEGIS CI job contexts are required status checks with strict up-to-date
  enforcement.
- Current blockers: none. **`--check-deferred M1` passes**: every obligation
  due at M1 is paid. AEGIS-227, AEGIS-233 and AEGIS-234 were discharged on
  2026-08-09 from the first real CI runs, and AEGIS-009 the same day from
  run 31295058007, whose `reproducibility` job executed
  `docs/ENVIRONMENT.md`'s canonical procedure verbatim on a clean
  `ubuntu-24.04` runner. What remains before M1 can close is promotion to
  `verified`, which needs an independent audit record — not an obligation.
- Deferred verification obligations: 5 open, listed in
  `docs/DEFERRED_VERIFICATION.md`; due M2=2, M3=1, M4=1, M5=1 — **none at M1**.
  Discharged: AEGIS-005 (exchange determinism), AEGIS-227, AEGIS-233,
  AEGIS-234 (first real CI runs) and AEGIS-009 (clean-machine reproducibility).

## M1 state

Implementation complete through slice 11 of `experiments/plans/M1.md` §5
(bench driver, evidence, docs, requirement statuses); slice 12 (closure gate)
remains. All 15 M1 requirements (AEGIS-027..041) are `implemented` — see
`requirements/implementation_status.json`. None is `verified`: promotion
requires an independent `/audit-milestone M1`, which has not run.

Built: exchange domain message vocabulary and wire codec
(`cpp/events/exchange_messages.{hpp,cpp}`, `wire.{hpp,cpp}`,
`sequence.hpp`), the three identifier spaces and price/quantity grid
(`cpp/exchange/order_book/types.hpp`, `instrument.{hpp,cpp}`), the central
limit order book with intrusive FIFO queues, a pre-sized order-ID index and
an injected-`memory_resource` level index (`cpp/exchange/order_book/**`),
the FIFO `MatchingEngine` covering limit orders, market orders (residual
termination, never rejected for liquidity), modify (in-place decrease and
cancel-replace), and the full reject matrix (`cpp/exchange/matching/**`),
the `Sequencer` and canonical `EventLog` (`cpp/exchange/sequencer/**`,
`cpp/exchange/state/**`), the debug-only invariant checker at both scopes
(`cpp/exchange/order_book/invariants.{hpp,cpp}`), snapshot/restore with
continuation equality (`cpp/exchange/state/snapshot.{hpp,cpp}`), the
`ExchangeNode` composition root, the `aegis_exchange_replay` CLI and
`aegis_exchange_bench` benchmark driver (`cpp/exchange/app/**`). All five
M1-dated architecture layers are populated;
`cpp-exchange-market-data` is re-dated to M3 (ADR-0012) and stays empty.

Five ADRs (0009-0013), `docs/EXCHANGE_CORE.md`, and the M1 rows of
`docs/LIMITATIONS.md`/`docs/RECOVERY_CONTRACT.md`/`docs/RUNBOOK.md`/
`docs/DEMO.md` are written. AEGIS-005's M1-dated obligation is discharged
(`experiments/evidence/AEGIS-005/exchange/`); AEGIS-237's residual is updated
to reflect the exchange-state recovery evidence, still blocked until M3 for
participant-state recovery. **AEGIS-009, 227, 233 and 234 remain open at
M1** — none is dischargeable by code; see "Owner actions outstanding" below
and `docs/LIMITATIONS.md`.

## M0 state

Implementation is complete and every stage of `scripts/ci_local.sh` passes (23
stages, post-remediation). Of the 22 M0 requirements, **12 are `verified` and 10
remain `implemented`** with their verification obligations registered.

Verified: AEGIS-001, 002, 003, 006, 007, 008, 010, 228, 231, 232, 235, 236.

Still `implemented` with an open obligation: AEGIS-004, 009, 229, 230, 237, 238.
Each one's frozen acceptance criterion names something that does not exist yet;
`tools/audit_requirements.py` refuses to promote them while the obligation is
open. AEGIS-005, 227, 233 and 234 have since been discharged.

See `experiments/milestone-reports/M0.md` for the full report and the audit record.

## Owner actions outstanding

The remote, the first CI runs and branch protection are **done** (2026-08-09),
discharging AEGIS-227, AEGIS-233 and AEGIS-234. Two items remain, neither of
them an M1 implementation blocker:

1. ~~**AEGIS-009 — a clean-machine reproducibility transcript.**~~ **Done
   2026-08-09.** The owner chose to pay this rather than re-date it.
   `docs/ENVIRONMENT.md` became one canonical procedure complete for a clean
   machine (the `apt` prerequisites and the two CMake options CI had been
   passing undocumented are now part of it), and the `reproducibility` CI job
   executes exactly that procedure on a clean `ubuntu-24.04` runner using the
   machine's own `python3`. Run 31295058007 passed: `check_environment.sh`
   green, debug and release both built, ctest 180/180 twice, all seven test
   layers passing. The environment record and the command transcript are
   registered under `experiments/evidence/AEGIS-009/`.
   `docker/Dockerfile.dev` stays unbuilt and out of scope — the frozen
   acceptance names no container.
2. **R8 from the M1 audit — move the owner-approval channel out of this file.**
   `tools/check_scope.py` reads scope exceptions from the
   "Owner-approved scope changes" line above, which the agent can write. The
   audit flagged it as self-service; it was left open deliberately and is not
   in any current task's scope.

Claude Code must update this file only when starting/closing a milestone. It must
not change the canonical specification.
