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
- Last audit: `/audit-milestone M0`, 2026-08-06, against `adf34ed` — 17 PASS, 5
  FAIL; the five blocking findings were remediated in R1–R4
  (see `experiments/milestone-reports/M0.md` §13). Closure was performed
  fast-track on the owner's instruction using that audit plus the completed
  remediation; **no second independent audit was run.** M1 has not yet been
  audited; `/audit-milestone M1` is required before any M1 requirement may be
  promoted to `verified`.
- Current blockers: none for implementation. Closure is blocked on
  `--check-deferred M1`, which fails on AEGIS-009/227/233/234 (owner actions,
  not code — see "Owner actions outstanding" below); every other gate passes.
- Deferred verification obligations: 9 open, listed in
  `docs/DEFERRED_VERIFICATION.md`; due M1=4, M2=2, M3=1, M4=1, M5=1.
  AEGIS-005's M1 obligation is discharged (was M1=5).

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

Still `implemented` with an open obligation: AEGIS-004, 005, 009, 227, 229, 230,
233, 234, 237, 238. Each one's frozen acceptance criterion names something that
does not exist yet; `tools/audit_requirements.py` refuses to promote them while
the obligation is open.

See `experiments/milestone-reports/M0.md` for the full report and the audit record.

## Owner actions outstanding

These are not M0 blockers — M0 is closed — but they are the events that
discharge four of the nine registered obligations, and they are what
`tools/audit_requirements.py --check-deferred M1` is waiting on before M1 can
close (see "Current blockers" above):

1. **Create the git remote** and push `milestone/m0-foundation` so
   `.github/workflows/ci.yml` executes for the first time (AEGIS-227, AEGIS-233,
   AEGIS-234).
2. **Protect the default branch** to require a passing workflow — this is the
   literal wording of AEGIS-234's acceptance criterion.
3. **Enable Docker Desktop WSL integration**, or run CI, so a clean-machine
   environment transcript exists (AEGIS-009).

Claude Code must update this file only when starting/closing a milestone. It must
not change the canonical specification.
