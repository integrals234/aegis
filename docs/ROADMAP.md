# Milestone Roadmap and Gates

## Rule

Claude Code implements exactly one milestone at a time. A milestone may not close until:
- its listed requirement IDs are at least `implemented`,
- acceptance tests exist and pass,
- the spec-auditor independently reviews the diff,
- `python3 tools/audit_requirements.py --milestone <ID>` passes,
- a milestone report is committed under `experiments/milestone-reports/`.

## M0 — Governance and Engineering Foundation

Deliver:
- repository/toolchain skeleton;
- frozen spec, requirements catalogue, status tracker and hooks;
- C++/Python build and test harnesses;
- CI, configuration, logging, sample-data and documentation policies;
- architecture dependency checks and ADR template.

Do not implement exchange or strategy behavior in M0.

## M1 — Deterministic Exchange Core

Deliver:
- typed events and IDs;
- baseline order book;
- FIFO matching;
- add/cancel/modify/market/limit behavior;
- partial/full fills, rejects, event log and snapshots;
- invariant checker, golden tests, property/fuzz tests;
- deterministic replay of exchange commands.

Gate: correctness and determinism only. No custom allocator or lock-free queue required yet.

## M2 — Futures and Replay Foundation

Deliver:
- futures metadata/schema;
- contract expiry/session handling;
- all roll policies and adjustment methods;
- roll audit and data-quality reports;
- replay modes, virtual clock and fault injection;
- Python bindings needed for data/replay.

## M3 — Participant, Reconstruction, Online Statistics and Execution

Deliver:
- participant-side snapshots/deltas/gap recovery;
- microstructure features;
- online rolling statistics with Python references;
- OMS state machine, portfolio/P&L;
- execution, latency, fees, slippage and queue approximation;
- adapter contracts.

## M4 — First Rigorous Strategy

Deliver one deep calendar-spread strategy:
- contract-aware spread construction;
- hedge ratio, stationarity, z-score;
- expiry/roll analysis;
- cost-aware execution;
- a reproducible experiment and limitations report.

No other strategy family is required to close M4.

## M5 — Independent Risk and Validation

Deliver:
- all pre-trade/runtime risk controls;
- portfolio analytics and kill switches;
- train/validation/test, walk-forward, expanding-window;
- cost/latency/slippage sensitivity;
- bootstrap, Monte Carlo, leakage tests;
- strategy rejection report.

## M6 — Multi-Strategy Research and Attribution

Deliver:
- cross-market statistical arbitrage;
- lead-lag analysis;
- volatility-regime strategy;
- optional microstructure execution research;
- portfolio/strategy/market/regime/execution attribution;
- gross-to-net and latency attribution.

## M7 — Decision Intelligence

Deliver:
- timed historical scenarios;
- all actions, confidence, scoring and Futures First mode;
- counterfactuals;
- calibration and behavioral analytics;
- copyable result reports and cohort-safe exports.

## M8 — Performance Engineering

Only now add:
- preallocated pools, arenas/free lists;
- intrusive structures where evidence supports them;
- bounded lock-free ingress/output;
- sharding, affinity and cache/layout work;
- rigorous median/p95/p99/p99.9/max benchmarks;
- baseline-vs-optimized equivalence and reports.

## M9 — Product Surface and Paper Path

Deliver:
- five dashboard workspaces;
- experiment registry and reproducible reruns;
- versioned API;
- live/delayed feed and paper-broker adapters;
- reconciliation, recovery, monitoring and daily reports;
- hard guard against production real-money endpoints;
- complete three-minute demo script.
