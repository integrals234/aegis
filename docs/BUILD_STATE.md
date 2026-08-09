# Build State

- Active milestone: M1
- M0 status: **CLOSED** 2026-08-06 (owner-approved fast-track closure)
- M1 status: **VERIFIED, AWAITING MERGE**. Started 2026-08-07 per
  `experiments/plans/M1.md` (approved plan of record, revised after nine owner
  corrections). All 15 M1 requirements (AEGIS-027..041) are `verified` as of
  2026-08-09, on an independent `/audit-milestone M1` against the final tree
  `2747ea6` — 15 PASS, 0 FAIL, no blocking finding. Every obligation due at M1
  is paid, so `--check-deferred M1` passes. What remains is merging PR #2; the
  milestone is not closed until it lands on `main`.
- Active branch: `chore/m1-closure` (PR #2 into `main`); the implementation
  branch `milestone/m1-exchange-core` merged as PR #1.
- Owner-approved scope changes: `scripts/ci_local.sh`, `.github/workflows/ci.yml`, `.github/workflows/governance.yml`, `scripts/governance_preflight.sh`
  — **the final use of this channel, which the same change retires.**
  (All four paths are on one line deliberately: `tools/check_scope.py` parses
  this line by prefix, so a wrapped continuation is silently not read — a
  brittleness worth noting given the line is now retired anyway.)
  R8 is remediated by ADR-0014: owner approvals now live in
  `configs/governance/policy.yaml` on protected `main`, and this line grants
  nothing once that mechanism is on `main`. The bootstrap pull request that
  installs it must nevertheless pass the *old* gate, and `.github/**` and
  `scripts/**` are not in M1's `allowed` list, so the retired channel is used
  once to introduce its own replacement. This is not Claude self-authorising:
  the R8 remediation was authorised by the owner in the M2 planning session on
  2026-08-09, and the owner ratifies it by approving the bootstrap pull request,
  which cannot merge without their review.
  The historical M1 entry this replaces read: `scripts/ci_local.sh`,
  `.github/workflows/ci.yml` — retargeting the hardcoded `--milestone M0` gate
  to M1 (`experiments/plans/M1.md` §2 gap 3, §5 slice 0).
- Last audit: **independent `/audit-milestone M1`, 2026-08-09, against the
  final tree `2747ea6` — 15 PASS, 0 FAIL, no blocking finding.** This is the
  audit of record behind the promotions; it supersedes the 2026-08-08 audit of
  `b33ca67`, which is not usable as the verification audit because the tree
  changed after it (R1–R7 remediation, CI remediation, and the AEGIS-009 /
  227 / 233 / 234 discharges). R1–R7 were confirmed present in the audited
  tree; R8 — the owner-approval channel living in this agent-writable file —
  remains deliberately open and is the one governance item M1 does not close.
  Three non-blocking observations were recorded: the benchmark artefacts were
  captured from a dirty worktree at `b33ca67`, the `kMalformedMessage`
  reject-matrix row proves a codec precondition rather than an emitted reject,
  and the `reproducibility` job builds debug and release but not `asan-ubsan`
  (which its own CI job covers). The earlier `/audit-milestone M0`,
  2026-08-06 against `adf34ed`, is recorded in
  `experiments/milestone-reports/M0.md` §13.
- CI is live. PR #1 merged to `main` as `ddc82f8`; the push-triggered run
  [31286449399](https://github.com/integrals234/aegis/actions/runs/31286449399)
  passed all nine jobs on `main`. `main` is protected by repository ruleset
  "Protect main" (id 20596537, active, `bypass_actors: []`, cannot be
  bypassed): pull request required, and **all ten** AEGIS CI job contexts are
  required status checks with strict up-to-date enforcement. The tenth,
  `Clean-machine reproducibility (AEGIS-009)`, was added on 2026-08-09 so the
  job that produces AEGIS-009's evidence cannot regress on `main` without
  blocking a merge.
- Current blockers: none. **`--check-deferred M1` passes**: every obligation
  due at M1 is paid. AEGIS-227, AEGIS-233 and AEGIS-234 were discharged on
  2026-08-09 from the first real CI runs, and AEGIS-009 the same day from
  run 31295058007, whose `reproducibility` job executed
  `docs/ENVIRONMENT.md`'s canonical procedure verbatim on a clean
  `ubuntu-24.04` runner. All 15 M1 requirements are now `verified` on the
  2026-08-09 final-tree audit, so the only step left is merging PR #2.
- Deferred verification obligations: 5 open, listed in
  `docs/DEFERRED_VERIFICATION.md`; due M2=2, M3=1, M4=1, M5=1 — **none at M1**.
  Discharged: AEGIS-005 (exchange determinism), AEGIS-227, AEGIS-233,
  AEGIS-234 (first real CI runs) and AEGIS-009 (clean-machine reproducibility).

## M1 state

All twelve slices of `experiments/plans/M1.md` §5 are complete, including the
closure gate. **All 15 M1 requirements (AEGIS-027..041) are `verified`**, on
the independent final-tree audit of `2747ea6` recorded above; each carries that
auditor, commit and date in `requirements/implementation_status.json`. The
catalogue now reads verified=27 (12 M0 + 15 M1), implemented=10,
not_started=201.

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
(`experiments/evidence/AEGIS-005/exchange/`); AEGIS-237's residual reflects the
exchange-state recovery evidence and stays blocked until M3 for
participant-state recovery. AEGIS-009, 227, 233 and 234 were all discharged on
2026-08-09 from real GitHub CI evidence. They remain `implemented` rather than
`verified` because they are M0 requirements whose lifecycle spans milestones —
discharging an obligation makes a requirement eligible, and promoting them is
M0's ledger to settle, not M1's.

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
2. ~~**R8 from the M1 audit — move the owner-approval channel out of this file.**~~
   **Remediated 2026-08-09 by ADR-0014**, in the bootstrap pull request that
   precedes M2. Owner approvals and the active milestone now come from
   `configs/governance/policy.yaml` on protected `main`, judged by
   `tools/governance/authoritative_check.py` running from `main` under
   `pull_request_target`; the line above is a historical record that grants
   nothing. Designing the fix found three further instances of the same defect
   that the audit had not named — `tools/check_frozen.py` shares the channel, so
   a frozen file and its own digest could be edited together; the scope policy
   is agent-writable; and `- Active milestone: M9` selects a tree-wide scope. All
   four are closed. A second boundary was also needed: the credential in the
   agent's environment was the owner's admin token and could rewrite the ruleset,
   so it is now a least-privilege GitHub App with no `Administration: Write`.
   The invariant is *no commit enters `main` without a fresh approving review
   from the separate owner identity* — deliberately not "Claude cannot merge",
   which is false. See ADR-0014 and `experiments/plans/M2.md` §5.

Claude Code must update this file only when starting/closing a milestone. It must
not change the canonical specification.
