# ADR-0026: Hedge-ratio and leakage conventions for the M4 calendar-spread signal

- Status: Accepted
- Date: 2026-08-17
- Requirement IDs: AEGIS-078, AEGIS-079, AEGIS-080
- Milestone: M4

## Context

AEGIS-078 asks for a documented static or rolling hedge-ratio estimator,
AEGIS-079 for a stationarity test with its assumptions and caveats stated
(Batch 2 content), and AEGIS-080 for leakage-free rolling entry/exit signals
whose timestamps use only contemporaneously available data. All three share
one underlying question this ADR answers once: what "leakage-free" means
precisely for this milestone's signal, and what window convention every
estimator built on it follows -- so `python/research/hedge_ratio.py`,
`python/research/signal_reference.py` and `cpp/participant/strategy/
calendar_spread_strategy.{hpp,cpp}` do not each answer it slightly
differently.

## Decision

**An observation is scored against the window as it stood *before* it, then
joins the window -- never against a window that already includes itself.**
This is `cpp/statistics/rolling_zscore.hpp`'s own pre-existing documented
convention (ADR-0022); M4 adopts it unchanged as the leakage rule for every
new estimator rather than inventing a second one. Concretely:

- `RollingZScore::push_and_score` (C++, unchanged) and
  `research.signal_reference.rolling_zscore_reference` (the new Python
  reference) both score value `i` from the window built out of observations
  `0..i-1` only, then push `i` into that window for `i+1`'s use.
- `research.hedge_ratio.rolling_hedge_ratio`'s ratio at index `i` is
  estimated from observations `[i - window, i)` -- strictly prior, never
  including `i`.
- Both report a defined, documented value rather than silently substituting
  one when there is not yet enough history: `0.0` for the z-score when the
  prior window has fewer than two observations or zero variance (unchanged
  from ADR-0022); `None` for the hedge ratio under the same conditions.

**The static hedge ratio is the OLS slope of far-on-near, via the
covariance/variance identity, computed from the two-pass textbook
definition.** Not an updating recursion: `python/common/offline_stats.py`
already established, for AEGIS-107, that the *reference* implementation
should be obviously correct by inspection against the definition rather than
fast or incremental, and `research.hedge_ratio.static_hedge_ratio` follows
that same discipline since it has no latency requirement of its own.

**The rolling hedge ratio's window is the caller's own choice, not a fixed
platform default.** Unlike the z-score signal (window supplied via
`CalendarSpreadConfig.zscore_window`), no frozen M4 requirement names a
specific hedge-ratio window; `rolling_hedge_ratio(observations, window)`
takes it as a parameter and validates only `window >= 2` (the minimum a
slope needs), leaving the actual value to each report or caller.

**Batch 2 discharges AEGIS-079 on top of this same convention**: a
stationarity test's own lookback window, wherever it is applied to score a
specific date, must not include that date's own observation either. This ADR
states the rule now so Batch 2 has nothing left to decide about leakage
itself -- only which stationarity test to run and how to report its
assumptions.

## Alternatives considered

- **Score against a window that includes the current observation** --
  rejected: this is precisely the leakage AEGIS-080's acceptance criterion
  ("signal timestamps use only contemporaneously available data") forbids,
  and it would silently disagree with `RollingZScore`'s existing, already-
  shipped convention.
- **An updating (Welford-style) hedge-ratio recursion** -- rejected for the
  *reference*, for the same reason ADR-0022 rejected it for
  `offline_stats.py`: the two-pass form is what makes agreement or
  disagreement with a compiled implementation a real finding rather than an
  artifact of sharing one algorithm.
- **A single platform-wide default window for every M4 estimator** --
  rejected: the z-score signal's window is a strategy-tuning parameter
  (`CalendarSpreadConfig`), the hedge ratio's is a research choice with no
  frozen requirement naming a value, and conflating the two would hide that
  difference behind one number.

## Consequences

- `python/research/hedge_ratio.py` and `python/research/signal_reference.py`
  need no revision when Batch 2's stationarity work lands; they already
  follow the convention that work depends on.
- A cross-language check between `signal_reference.py` and
  `RollingZScore` (deferred to Batch 2, through the existing
  `cpp-bindings -> cpp-statistics` edge) compares two implementations that
  are already known to share one leakage rule, so any divergence found is
  numerical, not conventional.
- Any later milestone's strategy signal reuses this same rule rather than
  re-deriving it.

## Verification

- `tests/cpp/unit/test_calendar_spread_strategy.cpp` -- the first
  observation, however extreme, scores `0.0` and never triggers an action;
  entry/exit sequencing matches hand-computed values.
- `tests/unit/test_signal_reference.py` -- the same documented edge cases,
  and the same six-value sequence
  `test_calendar_spread_strategy.cpp` uses, producing matching scores.
- `tests/unit/test_hedge_ratio.py` -- a rolling ratio at an index whose true
  relationship changed sharply from its own prior window is shown *not* to
  reflect that change, proving the current observation never entered its own
  window.

## Owner approval

Authorized under the owner-approved M4 plan of record, activated by PR #10,
2026-08-17.
