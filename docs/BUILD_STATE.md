# Build State

- Active milestone: M5
- M0 status: **CLOSED** 2026-08-06 (owner-approved fast-track closure)
- M1 status: **CLOSED** 2026-08-09. All 15 M1 requirements (AEGIS-027..041) are
  `verified` on an independent `/audit-milestone M1` against the final tree
  `2747ea6` — 15 PASS, 0 FAIL, no blocking finding. Every obligation due at M1
  was paid, so `--check-deferred M1` passes. PR #2 merged; canonical main
  `55ed162`.
- M2 status: **CLOSED** 2026-08-11. All 14 slices of `experiments/plans/M2.md`
  are complete. **20 of the 26 primary M2 requirements (AEGIS-011..026,
  054..063) are `verified`** on an independent spec-auditor review of the final
  tree; the other six carry owner-approved residuals (024→M4, 059→M9,
  060/061→M3, 062/063→M5) and stay `implemented`. Both inherited M2 obligations
  are discharged and `verified`: AEGIS-229 (bindings, slice 13) and AEGIS-230
  (columnar interchange, slice 5), so `--check-deferred M2` passes. AEGIS-237 is
  untouched and remains due at M3. PR #6 merged; canonical main
  `0a31251b140d698fd30ac7897b0a2d2760680c90`.
  Report: `experiments/milestone-reports/M2.md`.
- M2 was started 2026-08-10 per `experiments/plans/M2.md`
  (approved plan of record, rev. 4 — revised four times after the owner found
  the signed-anchor circularity, the credential gap, the fine-grained-PAT
  incompatibility and the `record_index` wording). All 14 slices are complete,
  and `milestone/m2-futures-replay` was merged and is no longer active.
- M3 status: **CLOSED** 2026-08-16. **33 of the 34 primary M3 requirements
  (AEGIS-064..075, 098..106, 108..119) are `verified`** on an independent
  spec-auditor review of the final tree; AEGIS-107 carries an owner-approved
  residual (→M8, latency/memory only) and stays `implemented`. All three
  inherited M3 obligations are discharged and `verified` — AEGIS-060
  (stale-data response), AEGIS-061 (feed recovery) and AEGIS-237
  (participant-state recovery) — so `--check-deferred M3` passes. PR #9 merged;
  canonical main `f149aacaa82a3a40294fd674b208740198d07232`.
  Report: `experiments/milestone-reports/M3.md`.
  `milestone/m3-participant-execution` was merged and is no longer active.
- M3 started 2026-08-12 per `experiments/plans/M3.md` (owner-approved plan of
  record, after the three architecture corrections recorded in that file's
  header — `configs/architecture_rules.yaml` protected as a governance path
  rather than exempted, `cpp-statistics` decoupled from participant book
  reconstruction, and the AEGIS-119 adapter design corrected to remove an
  implied production participant→exchange edge). PR #7 ("M3 activation
  policy") merged `configs/governance/policy.yaml`'s `active_milestone: M3`
  and the `m3-milestone-gate`/`m3-architecture-transition` approvals to `main`
  first, deliberately leaving this line at `M2` — the authoritative gate reads
  policy from the *base* branch and requires this mirror to match it, so a
  single PR flipping both would have failed its own gate. This is that first
  commit on `milestone/m3-participant-execution`, flipping the mirror now that
  the policy change is on `main`.
- M4 status: **CLOSED** 2026-08-18. All 6 primary M4 requirements
  (AEGIS-076..081) are `verified` on an independent spec-auditor review of the
  final tree, and both inherited obligations due at M4 — AEGIS-004 and
  AEGIS-024 — are discharged and `verified`, so `--check-deferred M4` passes.
  **PR #12 merged**; canonical main
  `596d9de140af509d059dd73a439da878ee10914c` (merge commit authored
  2026-08-18T12:31:05+05:30, committer `GitHub <noreply@github.com>` — the
  merge's own metadata, not an inferred date).
  Report: `experiments/milestone-reports/M4.md`.
  `milestone/m4-calendar-spread` was merged and is no longer active.
- M4 started 2026-08-17 per `experiments/plans/M4.md`, and was activated in
  **three** owner-merged steps rather than the planned two:
  1. **PR #10** ("M4 activation policy") merged
     `configs/governance/policy.yaml`'s `active_milestone: M4` and the
     `m4-architecture-transition`/`m4-build-wiring`/`m4-milestone-gate`
     approvals to `main`, deliberately leaving this mirror at `M3` — the
     authoritative gate reads policy from the *base* branch and requires the
     mirror to match it, so a single pull request flipping both would have
     failed its own gate, exactly as at the M3 transition (PR #7).
  2. **PR #11** ("M4 scope grant + in-scope half of Batch 1") added the
     `m4-participant-app-integration` approval **and** flipped this mirror to
     `M4`. The two had to land together: `check_layer_population` makes
     emptiness a checked fact in both directions, so flipping the mirror to
     `M4` in a tree whose three M4-dated layers were still empty failed
     `tools/check_architecture.py` by construction, while leaving the mirror
     at `M3` failed the authoritative gate. Only a tree carrying both the flip
     and the implementation satisfied both, so PR #11 also carried the part of
     Batch 1 already inside M4's declared scope, deliberately excluding the
     three `cpp/participant/app/participant_run.*` files its own approval
     grants — an approval takes effect only once merged, so the files it
     authorises could not legally land in the same pull request.
     `configs/milestone_scope.yaml` was **not** widened; the exact-path
     approval is the narrower mechanism R8 already provides.
  3. This branch, `milestone/m4-calendar-spread`, carries the remainder: the
     three participant-app files, the `--calendar-spread` demo, and Batch 2's
     research, reports and evidence.
- Active branch: `milestone/m5-risk-validation`, holding M5 Batch 1. PR #13
  (the M5 activation policy: `active_milestone: M4 -> M5`, five exact-path
  approvals) was merged first; this branch carries the mirror flip in the
  same commit as the first real sources in `cpp/participant/risk` **and**
  `python/validation` — `tools/check_architecture.py`'s
  `check_layer_population` makes emptiness a checked fact in both directions,
  and both layers are dated M5, so flipping the mirror in a tree where either
  was still empty would fail by construction.
- Owner-approved scope changes: `configs/architecture_rules.yaml`, `cpp/participant/CMakeLists.txt`, `cpp/participant/app/CMakeLists.txt`, `cpp/participant/app/risk_engine_gate.hpp`, `cpp/participant/app/risk_engine_gate.cpp`, `cpp/participant/app/participant_run.hpp`, `cpp/participant/app/participant_run.cpp`, `cpp/participant/app/participant_run_main.cpp`, `.github/workflows/ci.yml`, `scripts/ci_local.sh`, `scripts/check_cpp_style.sh`, `cpp/participant/app/fault_scenario.hpp`, `cpp/participant/app/fault_scenario.cpp`
  — **a MIRROR, not the authority.** Since ADR-0014 (R8, PR #3) this line
  **grants nothing**: approvals live in `configs/governance/policy.yaml` on
  protected `main`, where an agent cannot put them, and
  `tools/governance/authoritative_check.py` runs from `main` under
  `pull_request_target` reading this branch only as data. The thirteen paths
  above are transcribed from the five **M5** approvals in that policy
  (`m5-architecture-transition`, `m5-build-wiring`,
  `m5-participant-app-integration`, `m5-milestone-gate`,
  `m5-fault-scenario-risk-response`; all granted in PR #13). The eight M4
  paths this line previously carried became inert the moment
  `active_milestone` left M4: `approved_paths()` skips any approval whose
  milestone is not the active one, so they are dropped here rather than left
  to imply a permission that no longer exists.
  The mirror exists so `tools/check_scope.py` — retained as a fast advisory
  check, which still parses this line — agrees with the authoritative gate
  instead of contradicting it. If the two ever disagree, **the gate is right
  and this line is stale**: editing it changes nothing about what may merge,
  which is the whole point of R8.
  Historical record: M1 approved `scripts/ci_local.sh` and
  `.github/workflows/ci.yml` here to retarget the hardcoded `--milestone M0`
  gate (`experiments/plans/M1.md` §2 gap 3, §5 slice 0); the R8 bootstrap
  approved those two plus `.github/workflows/governance.yml` and
  `scripts/governance_preflight.sh` in the final use of this channel, which the
  same change retired.
- Last audit: **independent M2 spec-auditor review, 2026-08-11, against the
  final M2 tree.** Its first pass returned three blocking findings (AEGIS-056's
  missing rate metrics, AEGIS-057's missing interactive path, AEGIS-060's
  declared-but-unrealized delay); all three were remediated by building the
  missing behaviour rather than narrowing the wording, mutation-verified, and
  re-audited. The 20 M2 promotions rest on that review.
- Prior audit: **independent `/audit-milestone M1`, 2026-08-09, against the
  final tree `2747ea6` — 15 PASS, 0 FAIL, no blocking finding.** This is the
  audit of record behind the M1 promotions; it supersedes the 2026-08-08 audit of
  `b33ca67`, which is not usable as the verification audit because the tree
  changed after it (R1–R7 remediation, CI remediation, and the AEGIS-009 /
  227 / 233 / 234 discharges). R1–R7 were confirmed present in the audited
  tree; R8 — the owner-approval channel living in this agent-writable file —
  was left deliberately open at M1 and is **remediated 2026-08-09 by ADR-0014**
  (PR #3), before any M2 implementation depended on it.
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
  bypassed): pull request required, and **all eleven** AEGIS CI job contexts are
  required status checks with strict up-to-date enforcement. The tenth,
  `Clean-machine reproducibility (AEGIS-009)`, was added on 2026-08-09 so the
  job that produces AEGIS-009's evidence cannot regress on `main` without
  blocking a merge. The eleventh, `Authoritative governance gate (R8)`, was
  added by the owner on 2026-08-10 after it had been observed both passing a
  legitimate pull request (#3) and failing a deliberately tampered one (#4).
  The ruleset also requires one approving review, dismisses stale approvals on
  push and requires approval of the most recent push, so no commit reaches
  `main` without a fresh review from the owner identity — an identity the
  agent's GitHub App credential cannot produce (ADR-0014).
- Current blockers: none. **`--check-deferred M1` passes**: every obligation
  due at M1 is paid. AEGIS-227, AEGIS-233 and AEGIS-234 were discharged on
  2026-08-09 from the first real CI runs, and AEGIS-009 the same day from
  run 31295058007, whose `reproducibility` job executed
  `docs/ENVIRONMENT.md`'s canonical procedure verbatim on a clean
  `ubuntu-24.04` runner.
- Deferred verification obligations: **7 open** after M3, listed in
  `docs/DEFERRED_VERIFICATION.md`; due M4=2 (AEGIS-004, AEGIS-024), M5=3
  (AEGIS-062, AEGIS-063, AEGIS-238), M8=1 (AEGIS-107) and M9=1 (AEGIS-059).
  M3 discharged the three that were due at it — AEGIS-060, AEGIS-061 and
  AEGIS-237 — so `--check-deferred M3` passes, and registered exactly one new
  obligation, AEGIS-107→M8. Earlier milestones discharged AEGIS-229 and
  AEGIS-230 at M2, and AEGIS-005, AEGIS-227, AEGIS-233, AEGIS-234 and
  AEGIS-009 at M1. **Two obligations fall due at M4** and are that milestone's
  first-class closure work.

## M5 state

**IN PROGRESS** — PR #13 (the M5 activation policy) was approved and merged.
Batch 1 is underway on `milestone/m5-risk-validation`: the mirror above is
now `M5`, `cpp/participant/risk` carries a real `RiskEngine` and
`python/validation` carries the partition/walk-forward foundation. **Not yet
closed, and no requirement below is `verified` yet** — verification is
Batch-2-and-closure work, per `docs/BUILD_STATE.md`'s own promotion
discipline. This section will be replaced at closure with the final M5
state (`experiments/plans/M5.md`, `experiments/milestone-reports/M5.md`).

M5 is *Independent risk and validation*: 36 primary requirements
(AEGIS-120..138 risk and portfolio controls, AEGIS-139..155 anti-overfitting
validation) plus the three inherited obligations dated M5 in
`docs/DEFERRED_VERIFICATION.md` — AEGIS-062, AEGIS-063 and AEGIS-238. It turns
the mandatory risk seam into enforced policy. Before this batch,
`cpp/participant/risk` was empty and every risk verdict shipped
(`cpp/participant/app/participant_run.cpp`'s calendar-spread path,
M3's fixture-driven path) was the `AlwaysApproveRiskGate` test double
(ADR-0023). Batch 1 replaces that double in the calendar-spread demo path
with a real `risk::RiskEngine`; the M3 fixture-driven path
(`run_participant_fixture`) is unrelated to M5 and keeps its own double
unchanged, as ADR-0023 always intended.

### Batch 1 status (this commit)

Real, tested, focused-test-verified (not yet `verified` in the requirement
catalogue -- promotion is closure work):

* `cpp/participant/risk/`: `RiskEngine` implementing every AEGIS-121..137
  control (order quantity, position + reservation lifecycle, notional/FX,
  market/sector exposure, price collars, staleness/validity, idempotency,
  message-rate limits with a safety-cancel bypass, margin, leverage, daily
  loss and drawdown latches, volatility-triggered resizing, concentration/
  correlated-group limits, kill switches, connectivity loss, and the
  proposal/order audit-decision invariant).
* `cpp/participant/app/risk_engine_gate.{hpp,cpp}`: the `oms::RiskGate`
  adapter (ADR-0027).
* `cpp/participant/portfolio/portfolio_risk.{hpp,cpp}`: AEGIS-138 analytics
  (gross/net exposure, margin used/available, market/sector exposure,
  volatility/drawdown contribution, scripted stress scenarios).
* `cpp/participant/app/participant_run.*`: the calendar-spread demo now
  routes through the real engine; `aegis_participant_run --calendar-spread
  --stream PATH --risk-config PATH` is the production CLI path (Demo A).
* `cpp/participant/app/fault_scenario.*`: `run_market_stress_risk_scenario`
  pays the AEGIS-062 residual (real risk responses to spread-widening/
  volatility-spike/liquidity-vanish faults).
* `tests/cpp/unit/test_calendar_spread_risk_exchange_integration.cpp`: Demo
  B, the same engine and seam against a real unmodified M1 `ExchangeNode` and
  real FIFO matching -- allow, reject (zero submit/zero exchange order/zero
  portfolio change), resize, and global-kill-switch scenarios.
* `tests/cpp/unit/test_risk_fault_execution_stress.cpp`: pays the AEGIS-063
  residual (OMS/risk integration for kRejection/kLatencySpike/kPartialFill/
  kBackpressure).
* `python/validation/{partitions,walk_forward}.py`: the AEGIS-139/140/141
  foundation `check_layer_population` requires non-vacuously for
  `python-validation`.
* `configs/risk/limits.json` (+ `limits_reject_demo.json`, `stress.json`),
  ADR-0027, ADR-0028.
* Milestone gate retargeted M4→M5 in `.github/workflows/ci.yml` and
  `scripts/ci_local.sh`; `scripts/check_cpp_style.sh` parallelises the same
  full-tree clang-tidy file set (`AEGIS_TIDY_JOBS`, default `nproc`), proven
  by `tests/unit/test_check_cpp_style_parallel.py` against a stub toolchain.

Not yet done (Batch 2): AEGIS-139..155's remaining validation modules,
AEGIS-238's observability integration, evidence generation, and the
independent audit. `requirements/implementation_status.json` is untouched by
Batch 1 -- no status promoted, no obligation cleared.

### Batch 2 status (this commit)

Carries one small carry-in fix plus the full AEGIS-139..155 validation
suite and a full AEGIS-238 attempt. Real, tested, focused-test-verified --
not yet `verified` in the requirement catalogue; promotion is closure work.

* **Carry-in fix**: `app::RiskReleasingExecutionAdapter`
  (`cpp/participant/app/risk_engine_gate.hpp`) wraps the concrete execution
  adapter and releases a reservation automatically when `submit` returns
  `false` -- no manual `release_reservation` call needed for the ordinary
  submission-failure path. `cpp/participant/oms/**` remains untouched; the
  fix lives entirely in the composition root's own adapter choice.
  `release_reservation` stays public for its own separate use (manual
  reconciliation).
* `research.strategy_replay.ExecutionAssumptions`: fee/half-spread/slippage
  costs, decision/execution delay, and two fill assumptions (`TOUCH`,
  `CROSS_OR_NEXT`) that genuinely change fill timing/eligibility on the
  observation grid -- default value byte-identical to the pre-M5 signature.
* `python/validation/`: `stability.py` (142), `sensitivity.py` (143/144/145),
  `resampling.py` (146/147), `markets.py` (148), `regimes.py` (149),
  `baselines.py` (150/151), `leakage.py` (152/153), `roll_sensitivity.py`
  (154, reuses M4's module unmodified), `rejection.py` (155),
  `observability_harness.py` (AEGIS-238).

  **Correction (M5 closure).** An earlier version of this line claimed "the
  shuffled baseline receives a genuine `REJECT`". That was false. The
  independent M5 quant review found the AEGIS-155 evidence producer was
  judging the baseline using the *strategy's* cost sweep and bootstrap, plus
  a `min_round_trip_count` of 1,000,000 invented at the call site -- a
  manufactured verdict, not an earned one. With each subject judged on its
  own statistics and on `configs/validation/rejection_criteria.yaml`'s
  thresholds, the honest result on this dataset is the reverse: the
  **calendar-spread strategy is REJECTED** (unprofitable at the lowest swept
  cost; bootstrap CI excludes a positive mean) and the **shuffled baseline is
  ACCEPTED** (it happens to make +30.22 here). That result is preserved, not
  tuned away, and it is still the recorded verdict after the follow-up fix
  below.

  **Follow-up (M5 closure repair).** AEGIS-155's frozen acceptance -- "at
  least one *intentionally weak* strategy produces a rejection report" --
  was not yet demonstrated at the point above: the criteria demonstrably
  worked, but the only subject they rejected was the real strategy, not one
  weak by construction. A third subject,
  `intentionally_weak_concentrated_baseline`, was added: identical window,
  exit threshold, quantity, partitions, costs and execution assumptions as
  the real strategy's own config, differing only in `entry_threshold=3.0` --
  a standard, dataset-independent 3-standard-deviation statistical
  extremity, not a value searched against this series. Demanding that rare
  a signal structurally produces too few round trips (2, against the
  `min_round_trip_count=5` floor already in
  `configs/validation/rejection_criteria.yaml`) for the pre-existing
  `trade_concentration_too_few_round_trips` criterion in
  `evaluate_strategy_for_rejection` to trigger honestly -- no new rejection
  criterion or portfolio-concentration mechanism was added for this
  subject. The recorded verdict is **REJECT**, with
  `trade_concentration_too_few_round_trips` among the triggering criteria.
  A falsifiability test (`test_the_concentration_verdict_is_computed_not_
  hardcoded`) proves this is computed, not hard-coded: relaxing
  `min_round_trip_count` to the subject's own round-trip count flips the
  same result to ACCEPT. All three verdicts stand together, unaltered from
  each other: calendar-spread strategy REJECT, shuffled baseline ACCEPT,
  intentionally-weak-by-construction baseline REJECT.

  **B2, attempt 1 (this repair's first pass, since corrected):** the
  AEGIS-152/153 leakage detector was found to audit provenance
  *reconstructed from the documented windowing convention*, never connected
  to the real estimator's own execution. The first fix gave
  `rolling_zscore_reference` an optional `timing_sink` observer, but only
  `fitting_window_start_index` was read from live state --
  `fitting_window_end_index = index - 1` remained a constant expression, true
  for a correctly-behaving call but never actually checked. The claim written
  here at the time -- "a future regression... would not have been caught" (as
  now fixed) -- was **itself false**: the independent quant re-review proved
  it by mutating `rolling_zscore_reference` so the current observation joined
  the window before scoring (the canonical look-ahead bug) while leaving the
  provenance block untouched; the numeric scores changed (a real leak) but
  the detector still reported zero violations, because `end` never read the
  window that actually produced the score. The seeded-leaky counterpart had
  the mirrored defect: its "caught" result was driven by a hardcoded
  `end_index = index` literal, not by its seeded structural bug -- an
  ablation confirmed removing the bug while keeping that literal still
  produced the same 50/50 violations.

  **B2, attempt 2 (this fix).** `research.signal_reference` now tracks
  `(index, value)` pairs in its sliding window (`_execute_rolling_zscore`'s
  `consumed: deque[tuple[int, float]]`) instead of bare values, and BOTH
  `fitting_window_start_index` and `fitting_window_end_index` are read
  directly off that structure (`consumed[0][0]` / `consumed[-1][0]`) -- the
  same structure the mean/variance are computed from, so score and
  provenance cannot disagree. The honest path
  (`rolling_zscore_reference`) and the negative-test path
  (`rolling_zscore_reference_with_seeded_leak_for_falsifiability_check`)
  share one execution engine, differing only in whether the current
  observation joins the window before or after being scored -- so the
  leak's effect on both the numeric output and the emitted provenance is a
  consequence of shared arithmetic, not two independently hand-authored
  loops. Re-running the reviewer's exact attack against this version:
  numeric scores diverge under the leak exactly as before (confirming the
  leak is real), the honest path's provenance still passes with zero
  violations, and the leaky path's provenance is now caught in full (120/120
  violations on the AEGIS-152/153 evidence dataset). A provenance-integrity
  test (`test_provenance_reports_the_actual_consumed_window_not_a_feature_
  index_formula`) checks emitted boundaries against a hand-worked ground
  truth for a small fixture, independent of the production formula.
* `python/reports/`: `validation_report.py`, `rejection_report.py`,
  `portfolio_risk_report.py` (independently RECOMPUTES gross/net exposure
  from position/price accounting values and reconciles against the C++
  analytics' own report, rather than re-serializing it).
* `python/reports/report_model.py`: the M4 `code_commit` `-dirty`-suffix
  debt is fixed -- sibling `experiments/evidence/**` artifacts no longer
  mark the commit dirty, mirroring `tools/evidence_provenance.py`'s
  exclusion rule. M4 evidence is not regenerated; M4 stays closed.
* `configs/validation/{partitions,regimes,rejection_criteria}.yaml`.
* AEGIS-238: `tests/integration/test_participant_observability.py` drives
  the real `aegis_participant_run` binary and the real `RiskEngine` through
  a bounded outbound execution buffer this harness owns -- health, queue
  depth, dropped/backpressured events, latency and risk status are all
  non-vacuous in the recorded run. Evidence explicitly discloses this is
  the harness's own bounded buffer, **not** the M8 lock-free queue
  implementation, per the owner's activation-time authorization. The
  fallback re-deferral is **not** used; no ledger entry created.
* `tools/generate_validation_evidence.py` (17 artifacts, AEGIS-139..155),
  `tools/generate_observability_evidence.py` (AEGIS-238) -- Batch-2
  provisional evidence; closure regenerates all M5 evidence once, from a
  clean tree, at the reviewed commit.
* ADR-0029 (validation framework conventions).

Not yet done: the M5 closure pass itself (full CI matrix, independent
audit, promotion, milestone report). `requirements/implementation_status.json`
is untouched by Batch 2 -- no status promoted, no obligation cleared, the
AEGIS-238 fallback authorization is not exercised.

The five M5 approvals, all exact-path and milestone-scoped, so all expire when
M5 does:

1. `m5-architecture-transition` — `configs/architecture_rules.yaml`, for
   exactly one edge: `cpp-participant-app.may_depend_on +=
   cpp-participant-risk`. `cpp-participant-oms` already depends on
   `cpp-participant-risk`, so the reverse edge would be a cycle; the risk layer
   therefore implements no OMS interface and the `oms::RiskGate` adapter lives
   in the composition root. **No change to `cpp/participant/oms/**` is
   required by M5.**
2. `m5-build-wiring` — `cpp/participant/CMakeLists.txt` and
   `cpp/participant/app/CMakeLists.txt`, to add the risk subdirectory and the
   app's link edge.
3. `m5-participant-app-integration` — `cpp/participant/app/risk_engine_gate.hpp`,
   `risk_engine_gate.cpp`, `participant_run.hpp`, `participant_run.cpp`,
   `participant_run_main.cpp`. Named up front, because M4 had to spend an extra
   owner merge (PR #11) on composition-root paths omitted at activation.
4. `m5-milestone-gate` — `.github/workflows/ci.yml`, `scripts/ci_local.sh` and
   `scripts/check_cpp_style.sh`: the M4→M5 milestone-audit retarget in the
   workflow and its local mirror, plus parallelising the *same* full-tree
   clang-tidy file set. Full-tree tidy stays mandatory; a changed-files-only
   tidy is explicitly not authorised.
5. `m5-fault-scenario-risk-response` — `cpp/participant/app/fault_scenario.hpp`
   and `fault_scenario.cpp`, to add real M5 risk responses to the existing
   deterministic fault scenarios that AEGIS-062/063 need. No new fault kind, no
   `cpp/replay/**` change.

### AEGIS-238 — owner authorization recorded at activation

The owner has authorised the following, and nothing beyond it:

* **M5 must attempt full AEGIS-238 verification first.** No portion of it is
  deferred pre-emptively, and **this pull request performs no deferral**:
  `requirements/implementation_status.json` is untouched, and no ledger entry
  has been created. AEGIS-238 remains `implemented` with
  `verification_blocked_until: M5`, exactly as `main` already records it.
* The intended M5 producer for **queue depth** and **dropped/backpressured
  events** is the **bounded `ExecutionTransport` used by the integration
  harness** — the seam already reports backpressure by returning `false`, and
  M2's `kBackpressure` fault drives it deterministically.
* The evidence **must explicitly disclose** that this is the harness's bounded
  outbound buffer and **not** the M8 lock-free queue implementation
  (AEGIS-046/047/048, `cpp/queues`, which is empty and M8-dated). The residual
  registered against AEGIS-238 attributes queue depth to "M1's bounded queues";
  that attribution is inaccurate as to milestone, and M5 must not repeat it.
* **Only if** the final independent M5 auditor determines that this does not
  satisfy the queue-depth/dropped-events portion of frozen AEGIS-238 may that
  **residual portion alone** re-date to M8. That fallback is pre-authorised so
  it needs no mid-milestone owner round-trip; using it still requires the
  append-only ledger entry `tools/update_status.py` demands.
* If the fallback is never needed, **AEGIS-238 verifies fully at M5**.
* **No other new M5 deferral is owner-authorised.** Any other residual
  discovered during M5 goes to the owner before it is registered.

## M4 state

**CLOSED** 2026-08-18 — the closure pull request **#12** was approved by the
owner and merged. Canonical `main` is
`596d9de140af509d059dd73a439da878ee10914c`; the merge commit's own metadata
records 2026-08-18T12:31:05+05:30, committer `GitHub <noreply@github.com>`.
Plan of record: `experiments/plans/M4.md`; report:
`experiments/milestone-reports/M4.md`.

**All 6 primary M4 requirements (AEGIS-076..081) are `verified`**, and **both
inherited obligations due at M4 are discharged and `verified`** — AEGIS-004
(exchange/participant separation, whose rule was declared but vacuous while
`cpp/participant/strategy` was empty) and AEGIS-024 (roll-method sensitivity,
whose acceptance names *strategy* differences and so needed a strategy to
exist). `--check-deferred M4` passes. Catalogue: **93 `verified` / 10
`implemented` / 135 `not_started`**.

M4 delivers the first real strategy path: reconstructed near/far market state
→ `CalendarSpreadStrategy` (proposal-only) → the existing mandatory risk seam
→ OMS → portfolio → P&L, deterministic on both the production CLI path and a
test-only harness driving a real unmodified M1 `ExchangeNode` through real FIFO
matching. Activation took three merged steps: PR #10 (policy), PR #11 (scope
grant + mirror flip + the in-scope half of Batch 1), and
`milestone/m4-calendar-spread` itself (PR #12).

**Every price in M4 is synthetic**, and the two-sided quote stream is
constructed from daily OHLC/settlement bars, not observed tick data
(ADR-0025). No execution-quality, fill-realism or profitability claim is made
anywhere in the milestone.

Activation is complete under the four M4 approvals and no others:
`m4-architecture-transition` (one edge: `cpp-participant-app.may_depend_on +=
cpp-participant-strategy`), `m4-build-wiring`, `m4-milestone-gate` (PR #10) and
`m4-participant-app-integration` (PR #11). **No production
participant→exchange edge exists**, `cpp/participant/risk` is still empty (no
M5 risk policy) and there is no gateway (no M9 connectivity).

Closure verification was substantive. The independent audit **rejected the
first submission**, finding AEGIS-078 had no *historical* test despite its
frozen acceptance naming one, that all three Batch 2 reports pinned a digest
for a file the computation never read, and that AEGIS-079's report stated a
false fact about its own series. All three were fixed by building the missing
behaviour and correcting the disclosures rather than narrowing any wording; the
re-audit returned 8 PASS with no blocking finding.
`experiments/milestone-reports/M4.md` §10 lists every defect, including the
closure-review defect where observed far-leg prices were being discarded — the
cause of a spurious *zero* result in AEGIS-024.

## M3 state

**CLOSED** 2026-08-16 — the closure pull request **PR #9** was approved and
merged, making `f149aacaa82a3a40294fd674b208740198d07232` canonical `main`.
Plan of record: `experiments/plans/M3.md`; report:
`experiments/milestone-reports/M3.md`.

**33 of the 34 primary M3 requirements (AEGIS-064..075, 098..106, 108..119) are
`verified`** on an independent spec-auditor review of the final tree.
**AEGIS-107 stays `implemented`** with an owner-approved residual dated **M8**:
its frozen description names output, error, latency and memory; M3 completes
the output/error half (an algorithmically independent Python reference agrees
with the compiled C++ to 5.7e-14 against a 1e-9 tolerance, report committed)
but the latency/memory comparison needs performance-measurement infrastructure
M3 does not have, and `docs/BENCHMARK_POLICY.md` is frozen.

**All three inherited M3 obligations are discharged and `verified`:**
AEGIS-060 (stale-data response), AEGIS-061 (feed recovery for missing,
duplicated and sequence-gap faults independently) and AEGIS-237
(participant-state recovery across a real process boundary). So
`--check-deferred M3` passes.

Seven architecture layers were populated under the four-change
`m3-architecture-transition` approval from PR #7 and no others: add
`cpp-participant-app`; `cpp-bindings += cpp-statistics`;
`cpp-exchange-app += cpp-exchange-market-data`; narrow
`cpp-statistics.may_depend_on` to `[cpp-common]`. One consequential edit
accompanies the first — the new layer joins `mutable_globals_forbidden_in`,
which tightens rather than relaxes. **No production participant→exchange edge
exists.** ADR-0020 through ADR-0024 record the five decisions.

Closure verification was substantive rather than ceremonial. The independent
audit **rejected the first submission**, finding four requirements whose
components were built and unit-tested but never integrated — AEGIS-113's
latency model, AEGIS-116's fee/slippage model and AEGIS-117's missed-trade
tracker each had zero production callers — plus a false independence claim in
AEGIS-107's committed evidence, where the "independent" Python reference was a
line-for-line transliteration of the C++ (which is why every divergence was
exactly 0.0). All were fixed by building the missing integration and writing a
genuinely independent reference, not by narrowing the wording. A second audit
pass found three further gaps (a vacuous fee leg, two competing fee ledgers,
and AEGIS-117's uncomputed opportunity cost); those were fixed too. The first
full-policy `clang-tidy` run over M3 found 124 accumulated violations, all
fixed mechanically with no check disabled and no assertion weakened.
`experiments/milestone-reports/M3.md` §12 lists every defect.

## M2 state

**All 14 slices of `experiments/plans/M2.md` §8 are complete.** M2 delivers the
futures data stack and the deterministic replay core.

Built, `python/futures/`: contract identity and lifecycle (`identifiers`,
`contracts`, `chain`, `instruments`), trading-session calendars (`calendars`),
the normalized `futures_bar.v1` schema and ingestion with deterministic
`record_index` assignment (`schema`, `ingest`), data-quality detection
(`quality`), Arrow/Parquet/DuckDB interchange (`columnar`), four roll policies
(`roll/`), continuous series with difference and ratio adjustment (`series`),
the roll audit and roll-method comparison (`roll_audit`, `roll_sensitivity`),
and the unified feed boundary (`replay`).

Built, `cpp/replay/`: the canonical replay record and its total order
(`replay_event`), the fail-closed stream loader (`replay_stream`), a virtual
clock that structurally cannot read the system clock (`virtual_clock`), the
FNV-1a reproducibility manifest (`replay_manifest`), the engine with
cursor/resume (`replay_engine`), four pacing modes (`pacing`), eleven
deterministic fault kinds (`fault_injection`), and the CLI (`replay_run_main`).

`cpp-replay` has no dependency edge to any `cpp-exchange-*` layer in either
direction. `cpp-bindings` gained exactly one new edge, to `cpp-replay`, so the
binding surface can expose the canonical order and nothing more.

Determinism is proved **across separate OS processes**, matching the standard
this repository already applies to AEGIS-005: `aegis_replay_run` produces
byte-identical output across independent invocations, and a run resumed from a
cursor in a fresh process reproduces exactly the tail of an uninterrupted run.
No M2 figure is a timing measurement — the replay core never sleeps.

Five ADRs cover the milestone: 0015 (contract identity), 0016 (schema,
ingestion, interchange), 0017 (roll policies and adjustments), 0018 (replay
core, pacing, feed boundary) and 0019 (deterministic fault injection).

Closure verification found and fixed seven defects in the accepted slice-13
tree, the most consequential being five owner-approved residuals that had never
been registered — without an obligation the auditor would have permitted
promoting them to `verified`. Its root cause, a `tools/update_status.py` defect
that wrote an obligation without its ledger, is fixed and regression-tested.
`experiments/milestone-reports/M2.md` §8 lists all seven.

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
