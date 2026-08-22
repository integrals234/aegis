# Limitations

What AEGIS does not do, cannot currently claim, or enforces only partially. This
document exists because the alternative to writing limitations down is having
them inferred incorrectly from what the repository appears to contain.

Read it alongside [docs/DEFERRED_VERIFICATION.md](DEFERRED_VERIFICATION.md),
which lists the requirements whose verification is registered as outstanding.

## What exists at M0

M0 is the governance and engineering foundation. **There is no exchange, no
order book, no matching engine, no strategy, no risk engine, no OMS, no
portfolio, no research, no attribution, no Decision Arena and no dashboard.**
`docs/ROADMAP.md` places each of those in M1 through M9.

What exists is: governance tooling with tests, a C++20 and Python toolchain, a
layer-dependency checker, clock domains, a configuration system, structured
logging, a metrics registry, a message envelope with canonical encoding, a
determinism harness, minimal bindings, a schema registry, an experiment-manifest
schema, and CI.

## What exists at M1

M1 builds the deterministic single-instrument exchange core: sequencer,
central limit order book, FIFO matching, the full reject matrix, snapshot and
restore, a replay CLI, and benchmark workloads. **There is still no strategy,
no risk engine, no OMS, no portfolio, no research, no attribution, no
Decision Arena and no dashboard**, and no participant-facing market-data feed
(`cpp/exchange/market_data` stays empty until M3, ADR-0012). Full detail:
[docs/EXCHANGE_CORE.md](EXCHANGE_CORE.md).

**Deliberately absent from the exchange core, by design (ADR-0011):**

- **No self-trade prevention.** A participant can trade against its own
  resting order; nothing here detects or blocks it.
- **No auctions.** Every command matches immediately against the continuous
  book; there is no opening/closing auction or call-market phase.
- **No pro-rata allocation.** `MatchingPolicy` is an interface specifically
  so a later milestone could add one (AEGIS-040), but M1 ships FIFO only —
  `tests/cpp/unit/test_matching_policy_interface.cpp` proves the seam is
  real without shipping the second implementation.
- **No time-in-force beyond immediate matching.** Every limit order is
  effectively GTC; there is no IOC, FOK, or GTD.
- **Single-threaded, one writer per book.** `cpp/exchange/**` has no thread
  pool, no lock-free structure, and no sharding — AEGIS-047's multi-writer
  concurrency is M8 scope (`configs/architecture_rules.yaml` dates
  `cpp/memory`/`cpp/queues` to M8).
- **Client order IDs are reusable after termination.** Uniqueness is
  enforced over live orders only, scoped to `(participant_id,
  client_order_id)`; there is no unbounded tombstone set. See
  `docs/EXCHANGE_CORE.md`'s "Order lifecycle" section and ADR-0011.
- **The order-storage slab only grows.** `OrderStorage`'s slab
  (`cpp/exchange/order_book/order_storage.hpp`) never shrinks back to the
  allocator; a freed slot returns to an in-process free list, not to the
  OS. A book that once held N concurrent live orders keeps at least that
  much slab capacity for its process lifetime.
- **Benchmark timings are WSL2 figures, not comparable ones.** Every
  artifact under `experiments/evidence/AEGIS-036/` and
  `experiments/evidence/AEGIS-039/` carries `"local_non_comparable": true`
  and an `environment.virtualisation` block showing
  `bare_metal_claimable: false`. The *asserted* acceptance is operation and
  allocation counts, which are deterministic; no latency, throughput, HFT
  or production claim is derived from any M1 number
  (`docs/BENCHMARK_POLICY.md` rule 2; M8 owns tail-latency claims).

## Claims that cannot be made yet

| Claim | Why not |
|---|---|
| "AEGIS is deterministic" | M1 shows this for the exchange core specifically: `aegis_exchange_replay` on a committed scenario produces byte-identical canonical output across independent processes (`experiments/evidence/AEGIS-005/exchange/`). No participant, risk, OMS or strategy engine exists yet for the same claim to extend to. |
| Any latency, throughput or "fast"/"low-latency" figure | `experiments/evidence/AEGIS-036/` and `AEGIS-039/` record timings because `docs/BENCHMARK_POLICY.md` requires it, every one labelled `local_non_comparable` and WSL2. None may be quoted as a performance claim; M8 owns that work. |
| Strict complexity claims (`O(1)` lookup, `O(k)` matching) unqualified | Documented as *expected* O(1) lookup and O(k) in consumed resting orders (`docs/EXCHANGE_CORE.md`) — `configs/claims_policy.yaml` bans the unqualified "strict O(1)" wording. |
| Any trading result, Sharpe or drawdown | No strategy, no data, no backtest. |
| "Production", "HFT", "live" or "institutional-grade" | Simulation-only code; `docs/CV_CLAIMS_POLICY.md` forbids the phrasing, and `tools/check_claims.py` enforces it. |
| Test coverage percentage | No coverage gate is configured; no M1 requirement asks for one. |

## Controls that are partial by construction

### The specification hook is defence in depth

`.claude/hooks/protect_spec.py` sees only writes made through Claude's file
tools. **A shell redirect, `sed -i`, an editor, or `python -c` bypasses it
entirely**, and no amount of hardening changes that — the interception surface is
unbounded.

It has been hardened to fail closed on unparseable input, non-object payloads and
a missing project directory, and extended to cover
`requirements/frozen_hashes.json`. It is still not the control of record. The
authoritative controls are `tools/check_frozen.py` (hash **and** branch diff),
the pre-commit hook, and CI. See [adr/0006](../adr/0006-governance-evidence-milestone-control.md).

### Settings deny rules are a configuration assertion

`tests/unit/test_settings_deny_rules.py` asserts that `.claude/settings.json`
declares deny rules for `.env`, secrets, credentials and key material. Whether
the harness honours them is a property of Claude Code, not of this repository.
The behavioural control for committed secrets is `tools/scan_secrets.py`, which
scans the worktree, the index and git history.

### The mutable-globals check is a heuristic

`tools/check_architecture.py` detects file-scope mutable definitions by text, not
by parsing C++. It over-reports rather than under-reports. A false positive costs
a review comment; hidden global state costs a replay nondeterminism investigation
several milestones later.

### The C++ JSON Schema validator implements a subset

`cpp/common/config.cpp` supports `type` (including unions), `required`,
`additionalProperties` (boolean or schema), `enum`, `minimum`/`maximum`,
`minLength`/`maxLength` and `pattern`. Any other keyword is **reported as
unsupported** rather than ignored, because a validator that skips what it does
not understand accepts documents the schema author meant to reject. The Python
loader uses full JSON Schema draft 2020-12.

## Environment limitations

### No clean-machine transcript

`docker/Dockerfile.dev` describes the reproducible environment but **has not been
built**: Docker Desktop WSL integration is disabled on the M0 development host.
M0 therefore closes on local-virtualenv evidence, and AEGIS-009 carries a
registered obligation for a transcript from a container or CI runner.

### No CI run has happened

No git remote is configured and no workflow has executed. `.github/workflows/ci.yml`
is written and `scripts/ci_local.sh` runs the same stages locally, but AEGIS-234's
acceptance names *a passing workflow on the protected default branch*. Creating
the remote and the branch-protection rule are owner actions. AEGIS-227, AEGIS-233
and AEGIS-234 carry obligations for the same reason.

**These four obligations (AEGIS-009, 227, 233, 234) came due at M1 and are
still open.** None is dischargeable by M1's own code — M1 built the exchange
core, not CI infrastructure — and none was re-dated: re-dating without a
structural reason a later milestone can actually pay would be exactly the
kind of undocumented deferral the operating contract forbids. They remain
registered exactly as M0 left them; see `docs/DEFERRED_VERIFICATION.md` and
`docs/BUILD_STATE.md`'s "Owner actions outstanding" section.

### Development happens under WSL2

`tools/capture_environment.py` detects this and records
`virtualisation.bare_metal_claimable: false`. Per `docs/BENCHMARK_POLICY.md`
rule 2, any figure measured here must be labelled a WSL figure. Serious latency
work belongs on native Linux.

## Scope decisions recorded in ADRs

- **No price type yet.** Fixed-point scale, tick handling and rounding are
  decided in M1 with the order book that uses them ([adr/0002](../adr/0002-time-clocks-and-envelope.md)).
- **No columnar interchange yet.** Parquet/Arrow/DuckDB arrive with M2's data;
  building them now would require inventing the futures schema AEGIS-026 owns
  ([adr/0007](../adr/0007-data-interchange-and-experiment-manifest.md)).
- **No snapshot store.** Each module will own its persistence; a shared store is
  where the exchange/participant boundary would quietly dissolve
  ([adr/0008](../adr/0008-snapshot-and-recovery-contract.md)).
- **No domain metrics.** Queue depth, execution latency and risk status appear
  with their producers in M1, M3 and M5. A gauge nobody writes reads zero, and an
  operator believes the zero ([adr/0004](../adr/0004-config-logging-observability.md)).
- **C++20, not C++23.** Changing the language standard narrows the set of
  toolchains that can build AEGIS and is its own decision
  ([adr/0005](../adr/0005-toolchain-and-language-boundary.md)).

## M5 risk-model limitations

M5 (`cpp/participant/risk`, ADR-0027, ADR-0028) turns the mandatory risk seam
into real enforcement, but every control is deliberately simplified. None of
the following is a production risk system:

- **Margin is `margin_per_contract_units * abs(quantity)` (Model A), not
  SPAN.** No exchange clearing model, no portfolio-margining offsets between
  correlated legs, no intraday variation-margin call simulation.
- **Only one base currency is exercised against real data.** Every in-repo
  product (`configs/futures/products.yaml`) is `currency: USD`. The FX
  normalization mechanism exists and is tested against a synthetic non-USD
  fixture, but no real multi-currency market data validates it
  (`docs/DATA_AND_RESEARCH_POLICY.md`).
- **"Correlated exposure" is a config-supplied grouping, never an estimated
  correlation matrix.** `RiskLimitsConfig::concentration.correlated_groups`
  is set by whoever configures the engine; nothing in the decision path
  estimates correlation from observed data, deliberately (ADR-0028) --
  online estimation would make a risk decision depend on a statistic
  computed from the same stream the decision is about.
- **No risk state survives a process restart (R9).** Idempotency/
  duplicate-request protection is in-memory only, and so is everything else
  `RiskEngine` tracks: tripped kill switches, the daily-loss/drawdown
  latches, every leg/order reservation, the dedupe-key set, the drawdown
  high-water mark, and every `ProposalReleaseRecord` (staged/authorized/
  aborted/rejected/completed state). `ParticipantSnapshot` covers OMS and
  portfolio state only -- a fresh `RiskEngine` after a restart has no memory
  of any of the above. AEGIS-127's frozen acceptance does not require
  cross-process persistence, and no such persistence is claimed for any
  control, not idempotency alone. The `--fixture`/`--restore-from` recovery
  path (`docs/LIMITATIONS.md`'s next bullet) is the only place state is
  restored across a process boundary today, and it restores OMS/portfolio
  state through a test/fixture risk double, not a real `RiskEngine`.
- **`AlwaysApproveRiskGate` is reachable in the shipped binary via
  `aegis_participant_run --fixture` (R8).** Pre-existing from M3
  (ADR-0023's test/fixture double, `cpp/participant/app/participant_run.cpp`),
  driven by a `RecordedResponseAdapter` with no real exchange, and NOT
  reachable from the calendar-spread `--calendar-spread` path this file's
  M5 sections otherwise document -- but it is a risk-free order-submission
  path that exists in a shipped binary. Not fixed by M5's risk-engine work:
  closing it means replacing the fixture path's own composition (outside
  `cpp/participant/risk/**`'s surface), a decision left to a future,
  explicitly scoped turn rather than folded silently into this one.
- **A failed adapter submission releases its reservation through a
  composition-root decorator, not through the OMS itself.**
  `OrderManager::submit_new_order` (`cpp/participant/oms`, unmodified by M5)
  discards `ExecutionAdapter::submit`'s boolean return value, so the risk
  engine has no seam-level signal from the OMS that a send failed.
  `app::RiskReleasingExecutionAdapter` closes this without touching the OMS:
  it wraps the concrete adapter and releases the reservation automatically
  when `submit` returns `false`. A caller that constructs `OrderManager`
  with a *different*, non-wrapping adapter does not get this behaviour for
  free -- it is a property of the wrapper, not of `OrderManager` or
  `RiskEngine` in isolation. Proven by `tests/cpp/unit/
  test_risk_fault_execution_stress.cpp`'s
  `BackpressureAutomaticallyReleasesTheReservationThroughTheNormalLifecycle`.
- **`RiskEngine`'s own position bookkeeping duplicates `Portfolio`'s.**
  `cpp-participant-risk` may not depend on `cpp-participant-portfolio`
  (`configs/architecture_rules.yaml`), so the composition root must forward
  every fill to both `RiskEngine::on_fill` and `Portfolio::apply_fill`
  separately. Nothing detects a caller that forwards one and not the other.
- **Portfolio stress scenarios are scripted parallel price shocks, not a
  statistical simulation.** `portfolio::compute_portfolio_risk`'s
  `StressScenario` applies one uniform percentage move to every position's
  mark; `volatility_multiple`/`liquidity_factor` are reported as context,
  not separately modelled as execution-quality effects.
- **The demo's calendar-spread starting capital
  (`CalendarSpreadRunConfig::starting_capital_units`) is an arbitrary,
  documented constant**, not a claim about how much capital a real deployment
  would carry.
- **Risk-decision atomicity is guaranteed at the release epoch;
  exchange-execution atomicity is not (corrected, M5 closure repair R4).**
  Before ANY constituent order of a committed proposal is released,
  `RiskEngine::authorize_proposal_release` performs one whole-proposal
  authorization against current state (ADR-0027's "Correction 3"). That
  authorization is all-or-none: either every constituent becomes executable
  or none does, and afterwards `decide_order` computes no further
  proposal-level safety verdict. One integrity backstop remains: an order
  whose submitted instrument/side/quantity disagree with the exact
  economics staged and authorized is rejected `kIdentityMismatch` on its
  own, while its siblings are unaffected -- a per-leg outcome that CAN
  differ from a sibling's, even though it is not a new risk judgment (it
  consults no mutable safety state). Verified against the real composition
  root: staged and submitted economics are read from the same fields, so
  this backstop does not fire on that path today. After release, this
  system also has no basket/atomic multi-leg execution primitive: a
  transport or exchange failure on one leg can still leave the other filled
  alone. Neither residual is eliminated by this repair and neither is
  claimed to be.
- **`RiskEngine::abort_proposal_release` (M5 closure repair, R3) has no
  current call site in the calendar-spread demo.** It exists so a caller
  that authorizes a proposal and then, for a reason outside `RiskEngine`'s
  own visibility, does not submit one or more legs, can reclaim that
  capacity instead of stranding it forever. `participant_run.cpp`'s
  `execute_leg` has no failure mode that leaves a sibling leg's
  authorization stranded today, so this method is exercised directly by
  `tests/cpp/unit/test_risk_proposal_release_epoch.cpp`'s `ProposalAbort`
  suite, not by the demo's own composition.
- **A kill switch tripping after release authorization does not retract
  that authorization.** It blocks every SUBSEQUENT proposal, and live
  orders are handled by the existing emergency-cancel path -- but a
  constituent of an already-authorized proposal is not retroactively
  rejected, because rejecting it while a sibling was already sent is
  exactly the mixed verdict (and resulting naked leg) the release epoch
  exists to prevent. A kill switch tripping BEFORE the epoch rejects the
  whole proposal and releases nothing.
- **Cumulative limits are enforced on a proposal's FINAL NET PROJECTED
  state, not on every intermediate state its legs pass through.** A
  multi-leg proposal is judged as one intended portfolio transition: a
  proposal that adds exposure on one leg and reduces it on another is
  evaluated on the net result. Because execution is not atomic, if the
  adding leg fills before the reducing leg is sent, true instantaneous
  gross exposure can exceed what risk authorized. M5 does not claim
  worst-path or basket execution-risk protection; this is a documented
  semantic choice (ADR-0027 "Correction 3"), not an oversight.
- **Concentration is a share of the WHOLE portfolio, so a lone first
  position from a flat book is mathematically 100% concentrated.** A
  configured `max_concentration_share` below `1.0` therefore honestly
  rejects a strategy's very first position, unless other exposure already
  exists to share the portfolio with. This is the correct reading of
  "share of portfolio", not a bug or an under-tested edge case
  (`tests/cpp/unit/test_risk_engine_reservation_repair.cpp`'s
  `FlatBookConcentrationBelowOneRejectsTheFirstPositionHonestly` pins this
  behavior); no exception is invented for it. Both shipped risk configs
  (`configs/risk/limits.json`, `limits_reject_demo.json`) leave
  `max_concentration_share` at its disabling default (`1.0`) precisely
  because of this, so the demo's calendar-spread strategy never exercises
  a live concentration limit.

## M5 validation-framework limitations

M5 (`python/validation`, ADR-0029) builds the anti-overfitting framework
over synthetic data only. None of the following establishes a claim about
real markets:

- **Multi-market, regime and stability results are computed over
  deterministic synthetic series** (`validation._fixtures`), a seeded
  mean-reverting walk -- not observed prices for any of the three product
  families.
- **`ExecutionAssumptions`' fill assumptions (`TOUCH`, `CROSS_OR_NEXT`) are
  validation models of eligibility, not observed fills** -- there is no bid
  size, depth, or real order-book state behind either.
- **The bootstrap is i.i.d., not block**, over round trips: it assumes
  round trips are exchangeable, which a systematic drift across the sample
  window would violate (`validation.resampling`'s own `limitations` field,
  carried into every report).
- **AEGIS-155's "concentration" criterion is trade-count concentration
  (too few round trips), not AEGIS-134's portfolio instrument
  concentration.** The two are deliberately not conflated; validation does
  not recompute a risk-layer control.
- **Regime evaluation resets the rolling z-score window at each regime
  boundary** (ADR-0029): a regime's own reported outcome never benefits
  from data outside it, at the cost of discarding the window's warm-up
  history at every boundary.
- **AEGIS-238's queue depth and dropped/backpressured events come from the
  M5 integration harness's own bounded buffer
  (`python/validation/observability_harness.py`), not the M8 lock-free
  queue implementation** (`cpp/queues`, empty and M8-dated) -- disclosed in
  every AEGIS-238 evidence artifact per the owner's activation-time
  authorization (`docs/BUILD_STATE.md`).

## Data limitations

The only committed sample is synthetic, generated by `tools/make_sample_data.py`.
It describes no real instrument, venue or trading day, and no conclusion about
any market may be drawn from it. `configs/external_datasets.yaml` is empty: no
research has run, so registering a vendor feed would record an intention rather
than a fact.
