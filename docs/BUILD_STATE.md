# Build State

- Active milestone: M1
- M0 status: **CLOSED** 2026-08-06 (owner-approved fast-track closure)
- M1 status: **IN PROGRESS**, started 2026-08-07 per `experiments/plans/M1.md`
  (approved plan of record, revised after nine owner corrections).
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
- Current blockers: none
- Deferred verification obligations: 10 open, listed in
  `docs/DEFERRED_VERIFICATION.md`; due M1=5, M2=2, M3=1, M4=1, M5=1

## M1 state

In progress. Slice 1 (spine) is committed: exchange domain message vocabulary
(`cpp/events/exchange_messages.{hpp,cpp}`, `wire.{hpp,cpp}`, `sequence.hpp`),
the three identifier spaces and price/quantity units
(`cpp/exchange/order_book/types.hpp`), `InstrumentSpec` grid validation, the
central limit order book's `add`/`cancel` primitives with intrusive FIFO
queues and a level index, the FIFO matching *policy* (decision-only, no
mutation), the `Sequencer`, the canonical `EventLog`, and the `ExchangeNode`
composition root. All five M1-dated architecture layers
(`cpp-exchange-sequencer`, `cpp-exchange-order-book`, `cpp-exchange-matching`,
`cpp-exchange-state`, `cpp-exchange-app`) are populated in this one commit, as
`tools/check_architecture.py`'s layer-population check requires.
`cpp-exchange-market-data` is re-dated to M3 (ADR-0012) and stays empty.

Not yet built: the `MatchingEngine` that actually applies a command to a book
(accept/reject/trade/terminate orchestration), market orders, modify
semantics, the full reject matrix, the injected-allocator order index,
invariant checking, snapshot/restore, the replay CLI, and benchmarks. No M1
requirement is `implemented` or `verified` yet — see
`requirements/implementation_status.json`; statuses are updated once each
requirement's full acceptance criterion is met, not per slice.

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
