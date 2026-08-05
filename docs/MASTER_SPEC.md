# AEGIS — Canonical Master Specification

> **Frozen contract.** This document is the human-readable canonical specification for the project. Claude Code must not edit it. Any owner-approved scope change must be performed manually, followed by a new version and integrity hash.

## Product definition

AEGIS is a deterministic, event-driven, multi-market futures research, exchange-simulation, execution, risk, and trader-decision-intelligence platform. It must reconstruct futures contracts and markets, simulate exchange matching, research and validate strategies, model execution, enforce independent risk, attribute results, and train human decision-making under time pressure.

## Real-world purpose

AEGIS is intended for quantitative research, historical replay, futures-contract analysis, execution simulation, portfolio/risk testing, paper trading, trader training, recruitment simulations, and performance engineering. The initial project is not a production HFT venue, financial adviser, guaranteed-profit bot, or real-money client platform.

## Immutable architecture principles

1. Exchange simulation and proprietary-trading participant logic are separate systems connected only by versioned messages.
2. Strategies never bypass independent risk and OMS layers.
3. Correctness, determinism, auditability, and reproducibility precede optimization.
4. All research claims are out-of-sample, cost-aware, and evidence-backed.
5. All performance claims disclose benchmark methodology and tail latency.
6. Decision quality is distinct from realized outcome quality.
7. No feature is considered complete until its requirement ID is verified with evidence.

## Required modules and literal features

### Governance

- **AEGIS-001 — Frozen canonical specification:** The canonical project specification and requirement catalogue are immutable to Claude Code unless the owner explicitly enables specification editing.
- **AEGIS-002 — Requirement traceability:** Every required feature has a stable requirement ID, milestone, implementation status, and evidence list.
- **AEGIS-003 — Evidence-based completion:** No requirement may be marked complete using prose alone, TODOs, mocks, or unexecuted code.
- **AEGIS-004 — Exchange/participant separation:** Exchange-side sequencing and matching remain architecturally separate from participant-side feed handling, strategy, risk, OMS, and portfolio logic.
- **AEGIS-005 — Determinism before optimization:** Correctness and deterministic replay are mandatory before memory, concurrency, or latency optimizations.
- **AEGIS-006 — No fabricated claims:** All CV, README, performance, and trading-result claims must derive from committed reproducible evidence.
- **AEGIS-007 — One active milestone:** Only one milestone may be active in the primary implementation branch; later milestones cannot silently leak into scope.
- **AEGIS-008 — Decision records:** Material architecture changes require an ADR describing context, decision, alternatives, and consequences.
- **AEGIS-009 — Reproducible environments:** Build, test, data, and benchmark environments are versioned and reproducible.
- **AEGIS-010 — Secrets isolation:** Claude and source control are denied access to .env, credentials, private keys, and secrets directories.

### Futures Data & Contract Lifecycle

- **AEGIS-011 — Exchange and instrument metadata:** Store exchange, product, instrument, tick-size, multiplier, currency, timezone, and session metadata.
- **AEGIS-012 — Contract symbols and expiry metadata:** Represent individual futures contracts, expiry dates, first/last trade dates, settlement and delivery metadata.
- **AEGIS-013 — Trading-session calendars:** Represent sessions, holidays, maintenance windows, and overnight boundaries.
- **AEGIS-014 — Volume and open-interest ingestion:** Ingest and validate volume/open-interest fields used by roll policies.
- **AEGIS-015 — Fixed-days roll policy:** Roll a configurable number of sessions before expiry.
- **AEGIS-016 — Volume-crossover roll policy:** Roll when deferred-contract volume exceeds front-contract volume under configurable persistence rules.
- **AEGIS-017 — Open-interest-crossover roll policy:** Roll using configurable open-interest crossover rules.
- **AEGIS-018 — Liquidity-score roll policy:** Support a documented composite liquidity score for contract selection.
- **AEGIS-019 — Unadjusted continuous series:** Construct unadjusted continuous futures series while preserving raw contract prices.
- **AEGIS-020 — Difference-adjusted continuous series:** Construct additive back-adjusted series.
- **AEGIS-021 — Ratio-adjusted continuous series:** Construct multiplicative ratio-adjusted series.
- **AEGIS-022 — Return-preserving series:** Construct a return stream and optional synthetic index that avoids artificial roll jumps.
- **AEGIS-023 — Roll audit report:** Produce per-roll audit records with old/new contracts, prices, raw gap, trigger, and adjustment.
- **AEGIS-024 — Roll-method sensitivity:** Research can compare results across roll policies and adjustment methods.
- **AEGIS-025 — Bad-data detection:** Detect duplicates, gaps, invalid prices, impossible timestamps, and contract metadata conflicts.
- **AEGIS-026 — Normalized multi-market schema:** Normalize futures data from multiple products into one versioned schema.

### Deterministic LOB & Matching

- **AEGIS-027 — Price-time-priority order book:** Implement a central limit order book with deterministic FIFO priority within each price level.
- **AEGIS-028 — Limit orders:** Accept valid limit orders and rest or match them as appropriate.
- **AEGIS-029 — Market orders:** Execute market orders against available liquidity with explicit residual handling.
- **AEGIS-030 — Cancel requests:** Cancel resting orders by order ID without searching an entire price queue.
- **AEGIS-031 — Quantity modifications:** Support documented priority semantics for quantity reductions and increases.
- **AEGIS-032 — Price modifications:** Treat price change with explicit cancel-replace semantics.
- **AEGIS-033 — Partial fills:** Correctly update remaining quantity and emit fill events for partial executions.
- **AEGIS-034 — Full fills:** Remove fully filled orders and empty price levels.
- **AEGIS-035 — Rejected orders:** Reject malformed, duplicate, nonpositive, out-of-range, or otherwise invalid orders.
- **AEGIS-036 — Order-ID index:** Maintain expected O(1) order lookup using a pre-sized index.
- **AEGIS-037 — Intrusive FIFO price queues:** Store orders at a price using intrusive doubly linked queues or an equivalently allocation-free structure.
- **AEGIS-038 — Price-level index:** Use an explicitly documented tree, tick-array/bitset, or hybrid level index.
- **AEGIS-039 — Output-sensitive matching:** Matching complexity is documented as O(k) for k consumed resting orders.
- **AEGIS-040 — Configurable matching rules:** Architecture permits later FIFO/pro-rata variants without contaminating the initial FIFO core.
- **AEGIS-041 — Invariant checker:** Provide an expensive debug-only book invariant validator.

### Low-Latency Engineering

- **AEGIS-042 — Preallocated order pool:** Allocate order storage before the critical path.
- **AEGIS-043 — Arena/free-list reuse:** Recycle order nodes safely using an arena or free list.
- **AEGIS-044 — Cache-aware layout:** Document and benchmark structure layout, alignment, and hot/cold field separation.
- **AEGIS-045 — Compact event representation:** Use versioned compact event structures without unsafe ABI assumptions.
- **AEGIS-046 — Lock-free MPSC ingress:** Use a bounded lock-free multi-producer/single-consumer queue around the single-writer engine where justified.
- **AEGIS-047 — Single-writer matching core:** Exactly one writer mutates each order-book partition.
- **AEGIS-048 — Lock-free output queues:** Publish fills and market-data events through bounded queues with explicit backpressure policy.
- **AEGIS-049 — Instrument sharding:** Partition instruments across cores without sharing mutable books.
- **AEGIS-050 — Core-pinning support:** Provide optional Linux CPU affinity for benchmarks, not as a hidden requirement for correctness.
- **AEGIS-051 — No premature optimization:** Optimized implementations must be compared against a correct baseline.
- **AEGIS-052 — Tail-latency measurement:** Record median, p95, p99, p99.9, max, throughput, CPU, and allocations.
- **AEGIS-053 — Platform-specific benchmark disclosure:** Low-latency claims disclose hardware, OS, compiler, flags, logging, workload, and virtualization.

### Historical Replay

- **AEGIS-054 — Original-speed replay:** Replay timestamped historical events at original relative timing.
- **AEGIS-055 — Accelerated replay:** Replay at configurable speed multipliers.
- **AEGIS-056 — Fixed-rate replay:** Replay at a configurable event rate independent of original gaps.
- **AEGIS-057 — Step-by-step replay:** Advance one event or one timestamp group at a time.
- **AEGIS-058 — Deterministic benchmark mode:** Replay without wall-clock sleeps using a virtual clock.
- **AEGIS-059 — Unified strategy interface:** The same strategy API works with historical replay, simulated exchange, and paper-feed adapters.
- **AEGIS-060 — Fault injection: delayed data:** Inject deterministic data delay.
- **AEGIS-061 — Fault injection: missing/duplicate/gap:** Inject missing, duplicated, and sequence-gap events.
- **AEGIS-062 — Fault injection: market stress:** Inject spread widening, volatility spikes, and disappearing liquidity.
- **AEGIS-063 — Fault injection: execution stress:** Inject rejection, latency, partial-fill, and output-backpressure events.

### Market Data & Book Reconstruction

- **AEGIS-064 — Full-depth snapshots:** Load complete book snapshots into participant-side state.
- **AEGIS-065 — Incremental updates:** Apply ordered price- or order-level updates.
- **AEGIS-066 — Order-level reconstruction:** Support market-by-order reconstruction when data permits.
- **AEGIS-067 — Price-level reconstruction:** Support market-by-price reconstruction when only aggregated depth exists.
- **AEGIS-068 — Sequence validation:** Validate monotonic sequence numbers and detect gaps, resets, and duplicates.
- **AEGIS-069 — Stale-data detection:** Mark market state stale after configurable time or sequence conditions.
- **AEGIS-070 — Snapshot recovery:** Recover from a detected gap using a new snapshot and buffered deltas.
- **AEGIS-071 — Top-of-book metrics:** Calculate best bid/ask, spread, and mid-price.
- **AEGIS-072 — Microprice:** Calculate a documented quantity-weighted microprice.
- **AEGIS-073 — Depth and order-book imbalance:** Calculate configurable depth/quantity imbalance.
- **AEGIS-074 — Trade/cancellation intensity:** Calculate rolling trade and cancellation intensity.
- **AEGIS-075 — Queue depletion and adverse selection:** Track queue depletion signals and post-fill adverse-selection windows.

### Quantitative Research

- **AEGIS-076 — Calendar spread construction:** Construct near/far futures spreads with explicit contract and roll provenance.
- **AEGIS-077 — Term structure features:** Represent carry, contango/backwardation, expiry distance, and roll context.
- **AEGIS-078 — Calendar-spread hedge ratio:** Support documented static or rolling hedge-ratio estimation.
- **AEGIS-079 — Calendar-spread stationarity:** Test rather than assume spread stationarity.
- **AEGIS-080 — Calendar-spread z-score signals:** Generate leakage-free rolling entry/exit signals.
- **AEGIS-081 — Expiry and roll effects:** Evaluate how expiry proximity and roll dates affect calendar-spread behavior.
- **AEGIS-082 — Cross-market economic rationale:** Require an explicit economic rationale for every pair or basket.
- **AEGIS-083 — Rolling regression:** Estimate rolling relationships without future leakage.
- **AEGIS-084 — Hedge-ratio estimation:** Estimate and version hedge ratios used in spreads.
- **AEGIS-085 — Cointegration analysis:** Test cointegration where appropriate and document limitations.
- **AEGIS-086 — Spread stationarity analysis:** Test the constructed residual/spread for stationarity.
- **AEGIS-087 — Half-life estimation:** Estimate mean-reversion half-life when model assumptions are defensible.
- **AEGIS-088 — Structural-break detection:** Detect or flag breakdowns in historical relationships.
- **AEGIS-089 — Cross-market cost-aware signals:** Apply realistic spread, fees, slippage, and execution delays to stat-arb.
- **AEGIS-090 — Volatility-regime features:** Calculate realized volatility, range/ATR, autocorrelation, volume, spread, imbalance, and distance-to-extreme features as available.
- **AEGIS-091 — Regime classification:** Classify quiet, expansion, trend, reversal, and transition states using documented rules/models.
- **AEGIS-092 — Regime-conditioned trading:** Enable strategy only in historically supported regimes.
- **AEGIS-093 — Microstructure execution signal:** Optionally research queue imbalance, microprice, order flow, spread reversion, or adverse-selection signals.
- **AEGIS-094 — Lead-lag research:** Measure lagged cross-market relationships without claiming causation from correlation alone.
- **AEGIS-095 — Strategy plugin interface:** Strategies implement a stable interface and cannot bypass risk or OMS.
- **AEGIS-096 — Multiple strategy families:** Calendar spread, cross-market/stat-arb, and volatility-regime strategies remain separate first-class modules.
- **AEGIS-097 — Research reproducibility:** Every run stores config, data version, code commit, seed, date range, costs, and roll method.

### Online Statistics

- **AEGIS-098 — Rolling mean:** Update fixed-window mean without full recomputation.
- **AEGIS-099 — Rolling variance:** Update fixed-window variance with numerically stable add/remove logic.
- **AEGIS-100 — Rolling standard deviation:** Expose stable rolling standard deviation.
- **AEGIS-101 — Rolling covariance:** Update covariance online.
- **AEGIS-102 — Rolling correlation:** Update correlation online with zero-variance handling.
- **AEGIS-103 — Rolling z-score:** Compute leakage-free z-scores from prior/current window according to documented convention.
- **AEGIS-104 — Exponential statistics:** Support EW mean/variance with documented decay conventions.
- **AEGIS-105 — Realized volatility and beta:** Calculate realized volatility and rolling beta.
- **AEGIS-106 — Online drawdown and P&L moments:** Track high-water mark, drawdown, mean, variance, and higher moments where used.
- **AEGIS-107 — Numerical/performance validation:** Compare C++ online output, error, latency, and memory against Python reference.

### OMS & Execution

- **AEGIS-108 — Order lifecycle state machine:** Track Created, RiskPending, Rejected, Submitted, Acknowledged, PartiallyFilled, Filled, CancelPending, Cancelled, and Expired states.
- **AEGIS-109 — Market-order execution:** Model aggressive market execution.
- **AEGIS-110 — Passive limit execution:** Model resting limit orders and queue-dependent fills.
- **AEGIS-111 — Aggressive limit execution:** Model crossing limits with price caps.
- **AEGIS-112 — Cancel/amend lifecycle:** Model acknowledgements, races, and rejection outcomes for cancel/modify.
- **AEGIS-113 — Network and exchange latency:** Model configurable feed, decision, gateway, exchange, and acknowledgement latency.
- **AEGIS-114 — Partial fills:** OMS handles multiple fills and remaining quantities.
- **AEGIS-115 — Queue-position approximation:** Estimate volume ahead, cancellation assumptions, traded volume, and fill probability when order-level truth is unavailable.
- **AEGIS-116 — Fees and slippage:** Apply configurable fees, spread crossing, and slippage.
- **AEGIS-117 — Missed trades:** Record signals that were not filled and their opportunity cost.
- **AEGIS-118 — Position and cash accounting:** Maintain positions, average price, realized/unrealized P&L, and cash.
- **AEGIS-119 — Environment-independent OMS:** OMS connects to simulated exchange and later paper adapter through interfaces.

### Risk & Portfolio

- **AEGIS-120 — Mandatory risk path:** No strategy or Decision Arena action may submit directly to an exchange adapter.
- **AEGIS-121 — Maximum order quantity:** Reject or resize orders above per-instrument quantity limits.
- **AEGIS-122 — Maximum position:** Enforce long/short position limits.
- **AEGIS-123 — Maximum notional:** Enforce per-order and portfolio notional limits.
- **AEGIS-124 — Per-market and sector exposure:** Enforce market and grouped exposure limits.
- **AEGIS-125 — Price collars:** Reject orders beyond configurable reference-price collars.
- **AEGIS-126 — Stale-data rejection:** Block trading on stale or invalid market state.
- **AEGIS-127 — Duplicate-order protection:** Prevent duplicate client requests and replayed submissions.
- **AEGIS-128 — Message-rate limits:** Throttle or reject excessive order/cancel rates.
- **AEGIS-129 — Margin availability:** Estimate and enforce available margin using documented simplified or exchange-specific model.
- **AEGIS-130 — Maximum leverage:** Enforce leverage limits.
- **AEGIS-131 — Daily loss limit:** Stop or reduce trading after daily loss threshold.
- **AEGIS-132 — Maximum drawdown:** Stop or reduce trading after drawdown threshold.
- **AEGIS-133 — Volatility-triggered reduction:** Resize or reject risk as volatility rises.
- **AEGIS-134 — Concentration and correlation limits:** Limit concentrated or highly correlated portfolio exposures.
- **AEGIS-135 — Strategy and portfolio kill switches:** Provide idempotent strategy-level and global shutdown.
- **AEGIS-136 — Connectivity-loss response:** Define safe behavior on feed, exchange, or broker disconnect.
- **AEGIS-137 — Risk decision audit:** Emit approve, resize, or reject events with reason codes.
- **AEGIS-138 — Portfolio risk analytics:** Report gross/net exposure, margin, strategy/market risk, volatility and drawdown contribution, and stress results.

### Validation & Anti-Overfitting

- **AEGIS-139 — Train/validation/test separation:** Maintain explicit chronological or otherwise justified data partitions.
- **AEGIS-140 — Rolling walk-forward testing:** Train and test through rolling windows.
- **AEGIS-141 — Expanding-window testing:** Support expanding training windows.
- **AEGIS-142 — Parameter-stability surfaces:** Evaluate neighborhoods rather than only the best parameter point.
- **AEGIS-143 — Transaction-cost sensitivity:** Sweep fees, spread, and slippage assumptions.
- **AEGIS-144 — Latency sensitivity:** Sweep decision/execution delays.
- **AEGIS-145 — Slippage sensitivity:** Sweep slippage and fill assumptions.
- **AEGIS-146 — Bootstrap confidence intervals:** Bootstrap appropriate return/trade statistics with documented assumptions.
- **AEGIS-147 — Monte Carlo trade-sequence resampling:** Resample trade order to analyze drawdown/path risk.
- **AEGIS-148 — Multiple-market validation:** Evaluate hypotheses across relevant markets/contracts.
- **AEGIS-149 — Regime-specific evaluation:** Break performance down by market regime.
- **AEGIS-150 — Random-signal baseline:** Compare against a random or shuffled baseline where meaningful.
- **AEGIS-151 — Simple-rule baseline:** Compare against a simpler strategy.
- **AEGIS-152 — Look-ahead-bias detection:** Automated tests prevent future data use.
- **AEGIS-153 — Feature/data leakage checks:** Validate timestamps, fitting scope, and transformations.
- **AEGIS-154 — Roll-method sensitivity:** Compare research outcomes across futures roll conventions.
- **AEGIS-155 — Strategy rejection report:** System can formally reject a strategy for costs, instability, concentration, test failure, or drawdown.

### Performance & Execution Attribution

- **AEGIS-156 — Return and risk metrics:** Report total/annualized return, volatility, Sharpe, Sortino, Calmar, and maximum drawdown.
- **AEGIS-157 — Trade metrics:** Report win rate, profit factor, average win/loss, expectancy, turnover, and holding period.
- **AEGIS-158 — Market and contract attribution:** Break results down by market and contract.
- **AEGIS-159 — Strategy attribution:** Break results down by strategy.
- **AEGIS-160 — Regime attribution:** Break results down by regime.
- **AEGIS-161 — Long/short attribution:** Separate long and short performance.
- **AEGIS-162 — Session/time attribution:** Break results down by hour/session where timestamps support it.
- **AEGIS-163 — Entry/execution attribution:** Break results down by entry and execution method.
- **AEGIS-164 — Gross/net attribution:** Separate gross alpha from fees, spread, slippage, latency, and roll impact.
- **AEGIS-165 — Fill ratio:** Report fill ratio by order type and strategy.
- **AEGIS-166 — Average slippage and time-to-fill:** Report execution quality distributions.
- **AEGIS-167 — Adverse selection:** Measure post-fill price movement at configured horizons.
- **AEGIS-168 — Passive/aggressive comparison:** Compare passive and aggressive execution.
- **AEGIS-169 — Queue-model diagnostics:** Evaluate queue-position estimates where ground truth exists or on simulation.
- **AEGIS-170 — Cancellation/rejection rates:** Report cancellation and rejection statistics.
- **AEGIS-171 — Latency attribution:** Break total decision-to-ack/fill path into feed, strategy, risk, gateway, exchange, and response components.

### Trader Decision Arena

- **AEGIS-172 — Historical market scenarios:** Create scenarios from deterministic replay with information frozen at decision time.
- **AEGIS-173 — Visible market context:** Display recent price, bid/ask, depth, spread, volatility, position, P&L, and selected indicators.
- **AEGIS-174 — Timed actions:** Support BUY, SELL, HOLD, REDUCE, EXIT, and PASS under strict timers.
- **AEGIS-175 — Confidence capture:** Capture configured confidence levels such as 55/70/85/95 percent.
- **AEGIS-176 — Response-time scoring:** Measure response time using a monotonic clock.
- **AEGIS-177 — Action-quality scoring:** Score decisions using predeclared scenario policy/model rather than hindsight alone.
- **AEGIS-178 — Outcome scoring:** Separately record realized simulated outcome.
- **AEGIS-179 — Risk compliance scoring:** Penalize or reject decisions that violate risk rules.
- **AEGIS-180 — Overtrading detection:** Measure unnecessary action frequency.
- **AEGIS-181 — Passivity detection:** Measure missed high-confidence opportunities.
- **AEGIS-182 — Post-win/loss behavior:** Analyze decisions after gains and losses.
- **AEGIS-183 — Leading/trailing behavior:** Analyze risk-taking while score/P&L is ahead or behind.
- **AEGIS-184 — Regime consistency:** Analyze behavior across market regimes.
- **AEGIS-185 — Futures First scoring mode:** Provide levels ±1, ±2, ±3, ±4, bonus ±6, and pass 0.
- **AEGIS-186 — Copyable result report:** Generate compact reports containing score, speed, accuracy, risk, confidence, and level/category detail.
- **AEGIS-187 — Recruitment/cohort mode:** Support standardized scenario sets and anonymized cohort comparison without claiming employment prediction.

### Counterfactual Decision Intelligence

- **AEGIS-188 — Alternative-action simulation:** Compute outcomes for feasible alternative actions using the same future path and execution assumptions.
- **AEGIS-189 — Decision/outcome separation:** Maintain distinct decision-quality and outcome-quality scores.
- **AEGIS-190 — No hindsight leakage in decision score:** Decision quality uses only information available at decision time.
- **AEGIS-191 — Counterfactual explanation:** Explain chosen and alternative outcomes, assumptions, and uncertainty.
- **AEGIS-192 — Execution-aware counterfactuals:** Alternative actions use realistic fill/latency assumptions, not perfect prices.
- **AEGIS-193 — Uncertainty labeling:** Label model-dependent counterfactuals as estimates.

### Confidence & Behaviour Analytics

- **AEGIS-194 — Confidence-bin accuracy:** Compare declared confidence with empirical success by bin.
- **AEGIS-195 — Brier score:** Calculate Brier score for probabilistic decisions.
- **AEGIS-196 — Calibration error:** Calculate a documented calibration-error metric.
- **AEGIS-197 — Answer and pass rates:** Measure participation and pass behavior.
- **AEGIS-198 — Response time by confidence:** Report speed by confidence level.
- **AEGIS-199 — Risk after wins/losses:** Measure risk-taking following wins and losses.
- **AEGIS-200 — Overconfidence and underconfidence:** Detect statistically meaningful calibration direction with caveats.
- **AEGIS-201 — Action consistency:** Measure consistency for materially similar scenarios.
- **AEGIS-202 — Regime-specific weakness:** Report calibration and performance by regime.
- **AEGIS-203 — Recruitment/training usage:** Support personal development, coaching, competitions, and assessment while avoiding deterministic hiring claims.

### Dashboard & Experiment Management

- **AEGIS-204 — Market Replay workspace:** Choose instrument/date, control replay, inspect book/trades, and inject faults.
- **AEGIS-205 — Research Lab workspace:** Configure strategies, markets, periods, and experiments.
- **AEGIS-206 — Risk Console:** Display positions, P&L, margin, exposures, breaches, and kill-switch status.
- **AEGIS-207 — Decision Arena workspace:** Run timed scenarios and show result analytics.
- **AEGIS-208 — Performance Lab:** Display strategy, execution, latency, risk, and robustness analytics.
- **AEGIS-209 — Experiment ID:** Assign immutable unique IDs.
- **AEGIS-210 — Git commit capture:** Record code commit for every experiment.
- **AEGIS-211 — Data version capture:** Record dataset/version identifiers.
- **AEGIS-212 — Configuration capture:** Save full resolved configuration.
- **AEGIS-213 — Date/contract/roll capture:** Record date range, contracts, and roll method.
- **AEGIS-214 — Cost and seed capture:** Record cost assumptions and random seed.
- **AEGIS-215 — Artifact registry:** Register reports, logs, metrics, and plots by experiment.
- **AEGIS-216 — Re-run command:** Generate a reproducible rerun command.
- **AEGIS-217 — Backend/frontend separation:** UI consumes versioned APIs rather than reaching into engine internals.
- **AEGIS-218 — Accessibility and usability:** Core dashboard and Decision Arena flows are keyboard-usable and expose clear timer/status states.

### Paper Trading Path

- **AEGIS-219 — Live/delayed market-data adapter:** Add a versioned adapter without changing strategy interfaces.
- **AEGIS-220 — Broker paper adapter:** Submit only to a paper/sandbox environment.
- **AEGIS-221 — Position reconciliation:** Reconcile internal and broker-paper positions.
- **AEGIS-222 — Restart recovery:** Recover OMS/portfolio state from snapshots/events.
- **AEGIS-223 — Connectivity monitoring:** Detect disconnects and invoke safe behavior.
- **AEGIS-224 — Daily paper report:** Generate positions, trades, P&L, risk, and anomalies.
- **AEGIS-225 — No real-money execution:** Initial project has no enabled production trading path.
- **AEGIS-226 — Same strategy interface:** Strategies run unchanged across replay, simulation, and paper environments.

### Engineering Platform

- **AEGIS-227 — Modern C++ toolchain:** Use C++20 or newer, CMake presets, strict warnings, sanitizers, static analysis, and tests.
- **AEGIS-228 — Python research toolchain:** Use a pinned Python environment for data, research, validation, and reports.
- **AEGIS-229 — C++/Python bindings:** Use pybind11 or a documented equivalent for selected engine APIs.
- **AEGIS-230 — Columnar data interchange:** Use Parquet/Arrow and DuckDB where suitable with versioned schemas.
- **AEGIS-231 — Configuration system:** Use versioned validated YAML/TOML/JSON configuration.
- **AEGIS-232 — Structured logging:** Emit machine-readable logs with event/experiment correlation IDs.
- **AEGIS-233 — Unit, integration, property and replay tests:** Maintain distinct test layers.
- **AEGIS-234 — Continuous integration:** CI runs spec audit, build, tests, static checks, and secret scanning.
- **AEGIS-235 — Documentation:** Architecture, methods, assumptions, limitations, runbooks, and demo steps remain current.
- **AEGIS-236 — Sample data policy:** Commit only small legally redistributable samples; large/licensed data stays external.
- **AEGIS-237 — Failure recovery:** Critical stateful services document shutdown, snapshot, and recovery behavior.
- **AEGIS-238 — Observability:** Expose health, queue depth, dropped/backpressured events, latency, and risk status.

## Canonical delivery order

M0 Governance and engineering foundation → M1 deterministic exchange core → M2 futures/replay foundation → M3 participant, online statistics, reconstruction and execution → M4 first rigorous calendar-spread strategy → M5 risk and validation → M6 multi-strategy research and attribution → M7 decision intelligence → M8 performance engineering → M9 dashboard and paper-trading path.

## Definition of done

The overall project is complete only when every MUST requirement is `verified`, all evidence paths exist, all required tests and reproducibility checks pass, benchmark and research reports disclose their assumptions, and a clean-machine demo can execute the documented end-to-end flow.