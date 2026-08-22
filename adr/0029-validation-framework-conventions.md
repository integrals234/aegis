# ADR-0029: Validation framework conventions

- Status: Accepted
- Date: 2026-08-18
- Requirement IDs: AEGIS-139, AEGIS-140, AEGIS-141, AEGIS-142, AEGIS-143,
  AEGIS-144, AEGIS-145, AEGIS-146, AEGIS-147, AEGIS-148, AEGIS-149,
  AEGIS-150, AEGIS-151, AEGIS-152, AEGIS-153, AEGIS-154, AEGIS-155
- Milestone: M5

## Context

M5's anti-overfitting framework (`python/validation`) has to make a series
of choices the frozen requirement titles name but do not specify: what
"execution assumptions" mean concretely, how a look-ahead detector avoids
becoming a second copy of the estimator it is meant to check (the AEGIS-107
lesson -- see `docs/BUILD_STATE.md`'s M3 state and
`experiments/milestone-reports/M3.md`), and what a formal rejection verdict
is actually allowed to depend on.

## Decisions

**`ExecutionAssumptions` is a validation execution model, not a claim of
market realism.** Added to `research.strategy_replay` (not a second replay
engine) as an optional parameter defaulting to zero-delay/zero-cost/`TOUCH`,
byte-identical to the pre-M5 signature. Delay genuinely shifts which
observation a signal fills against (`execution_index`), never a number
subtracted from a report afterward; a delayed signal with no eligible
observation is dropped, never fabricated. Two fill assumptions (`TOUCH`,
`CROSS_OR_NEXT`) differ in eligibility, not merely in label.

**Partition discipline: chronological splits plus an enforced lock, not a
convention.** `validation.partitions.guard_test_set_access` raises
`LockedTestPartitionError` for a `tuning`-purpose access to the test split;
`DatasetPartitions.get` calls it, but a caller reading `.test` directly
bypasses it -- the guard is a callable primitive an experiment's own code
must invoke at its actual data-access point, not a runtime sandbox.

**Walk-forward/expanding-window folds enforce `train_end < test_start` by
construction**, not by a check run afterward: the index arithmetic that
builds each fold cannot produce an overlapping pair.

**Regime evaluation resets the rolling window at each regime boundary.**
Each regime is replayed as a self-contained series (`validation.regimes`);
letting the signal window straddle a boundary would let the adjacent
regime's data silently influence the regime being reported on.

**The look-ahead/leakage detector consumes recorded TIMING metadata, never
the estimator's arithmetic (the AEGIS-107 lesson, applied here).**
`validation.leakage.FeatureTimingRecord` carries only a feature's own index
and the index range of data used to compute it.
`honest_rolling_zscore_timing_records` derives these from the *documented*
windowing convention ("scored against the prior window only") -- structural
metadata about a promise, never a recomputed z-score value. This is what
makes `seeded_leaky_timing_records` a genuine, catchable defect rather than
a detector that would agree with a bug by re-deriving it the same way.

**Baselines run the identical entry/exit/cost/fill machinery, scored by a
different signal.** `validation.baselines` reuses `replay_strategy`'s exact
transition and cost logic (`execution_index`, transaction costing) rather
than reimplementing a parallel trading loop, so a baseline's result is
comparable to the strategy's on every axis except the signal itself.

**Resampling: bootstrap and Monte Carlo are two different mechanisms, not
the same code renamed.** Bootstrap resamples round trips WITH replacement
to build a confidence interval on their mean; Monte Carlo PERMUTES the
realized trade order (every trade used exactly once per path) to
characterize path/drawdown risk, which a with-replacement resample cannot
represent. Both use an explicit `random.Random(seed)` instance, never
module-level RNG state, so identical input and seed reproduce byte-identical
output.

**Bootstrap uses i.i.d., not block, resampling**, because the sample unit
(one completed round trip) is already a discrete, non-overlapping event
rather than a continuous return series with autocorrelation a block
structure exists to preserve. The stated limitation is that round trips are
still treated as exchangeable, which a systematic drift across the sample
window would violate.

**Multi-market validation reads product families from the canonical
config, never a hardcoded list** (`validation.markets.
configured_product_roots` parses `configs/futures/products.yaml`), and every
configured market appears in the report including a zero-trade outcome.

**Roll-method sensitivity reuses M4's own module unmodified**
(`research.roll_method_sensitivity`); `validation.roll_sensitivity` adds
only a "report the actual difference, including a genuine zero" framing --
the M4 closure lesson (`experiments/milestone-reports/M4.md` section 10:
a spurious zero was once a real defect, not something to paper over by
manufacturing a nonzero difference in either direction).

**Formal rejection evaluates every configured criterion and records all of
them, triggered or not.** The five categories the plan of record names are
read at the layer where they are actually computable: transaction costs
(break-even from the cost sweep), parameter instability (dispersion across
the stability surface), trade concentration (read at the research layer as
"too few round trips to trust the other statistics," distinct from
AEGIS-134's portfolio instrument concentration, which this layer does not
recompute), statistical/test failure (the bootstrap CI excluding a positive
mean), and drawdown (the actual realized trade sequence's own maximum
drawdown, distinct from the Monte Carlo drawdown *distribution*). At least
one intentionally weak strategy -- the AEGIS-150 shuffled-signal baseline,
run through the identical pipeline -- must produce a genuine `REJECT`; nothing
here special-cases a hardcoded verdict.

## Alternatives considered

- **A second, independent strategy implementation to serve as the
  "leakage-free reference."** Rejected: this is exactly the AEGIS-107
  failure mode generalized -- a second implementation of the same logic
  agrees with the first by construction on anything both authors got wrong
  the same way.
- **Estimating FX rates, correlations, or regimes adaptively from the
  data.** Rejected throughout M5 (echoing ADR-0028): no real market data
  exists in this repository to estimate any of these from honestly.

## Consequences

- Every validation report carries the same data-honesty disclosure
  (`python/reports/validation_report.py`'s `_DATA_HONESTY_DISCLOSURE`):
  synthetic data, no live-profitability or production-risk-adequacy claim.
- `python/validation` depends only on `python-common`, `python-data`,
  `python-futures`, `python-research` (`configs/architecture_rules.yaml`,
  unchanged by M5) -- no new architecture edge was needed for any module in
  this ADR.

## Verification

One test file per module in `tests/unit/test_validation_*.py`, plus
`tests/unit/test_execution_assumptions.py` (the delay/fill mechanism
directly) and `tests/unit/test_validation_reports.py` (report rendering and
the portfolio-risk reconciliation). The leakage detector's falsifiability is
the one negative-gate-style proof in this set: `test_validation_leakage.py`
asserts the honest path passes AND the seeded leaky fixture is caught.

## Owner approval

Implied by merged `m5-architecture-transition`/`m5-participant-app-integration`
(PR #13's activation policy); this ADR is filed alongside the implementation
it documents.
