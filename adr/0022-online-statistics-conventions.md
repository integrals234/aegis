# ADR-0022: Online statistics — numerical conventions and the Python reference boundary

- Status: Accepted
- Date: 2026-08-12
- Requirement IDs: AEGIS-098, AEGIS-099, AEGIS-100, AEGIS-107
- Milestone: M3

## Context

AEGIS-098–100 (this slice) and AEGIS-101–107 (a later M3 slice) ask for
fixed-window rolling statistics that update incrementally rather than
recomputing from scratch, with AEGIS-099 specifically requiring "numerically
stable add/remove logic" and AEGIS-107 requiring a cross-language report
comparing the C++ implementation against a Python reference. Two decisions
have to be made before any code is written: what "numerically stable" means
concretely for a *sliding* (not merely expanding) window, and what the C++
implementation's public surface looks like so it stays reusable at M4–M7
(ADR-0020) and bindable for AEGIS-107 without dragging in anything else.

## Decision

**Fixed-window mean and variance use the reverse-Welford update.** Adding a
point to a Welford accumulator (`n`, `mean`, `M2`) is the standard one-pass
stable algorithm. Removing the oldest point when a window is full is the
algebraic inverse, derived directly from the forward update rather than
approximated:

```
mean_prev = (n * mean - x_removed) / (n - 1)
M2_prev   = M2 - (x_removed - mean_prev) * (x_removed - mean)
```

This is exact under the same floating-point model the forward Welford update
already uses — not a heuristic — and is what makes eviction from a sliding
window numerically stable rather than a naive `sum -= x; sum_sq -= x*x`, which
loses precision catastrophically as the window slides across a nonzero mean.

**Sample variance, `ddof = 1`, documented explicitly.** `variance()` divides
`M2` by `n - 1`; a window with fewer than two observations reports `0.0`
rather than dividing by zero or `NaN` — an explicit, tested edge case, not an
accident of the formula.

**The library's public surface is plain numeric observations only.**
`RollingMoments::push(double)` takes a bare `double`; nothing in
`cpp/statistics` names a book, order, feed, or participant type. This is what
`cpp-statistics.may_depend_on = [cpp-common]` (ADR-0020) means concretely, and
it is what makes the type directly bindable: `cpp-bindings` can expose
`push`/`mean`/`variance`/`stddev` without knowing anything about the
participant pipeline that eventually feeds it real numbers.

**C++ is production; the Python side is two modules with two different jobs.**

`python/common/online_stats.py` implements the *same* reverse-Welford
recursion as the C++, step for step. It is an executable specification and a
check that the compiled binding layer transports values faithfully — but it
is **not** an independent validation of the recursion, because it *is* the
recursion. This ADR originally claimed it was independent, and AEGIS-107's
cross-language report rested on that claim; the M3 closure audit found the
module to be a line-for-line transliteration (identical variable names,
identical branch structure), which is why every reported divergence was
exactly `0.0`. Agreement between a transliteration and its source is not
evidence about the source. **That claim is withdrawn here.**

`python/common/offline_stats.py` is the genuinely independent reference, and
is what AEGIS-107's numerical claim now rests on. It computes every quantity
**directly from its textbook definition** using deliberately different
algorithms: two-pass variance and covariance rather than any updating form,
the exponentially-weighted mean expanded as an explicit weighted sum over the
whole history rather than as a recurrence, and drawdown by plain scan. These
are slower and allocate freely — that is the point. They are obviously
correct by inspection against the definition, so a disagreement is a real
finding about the production recursion, and agreement is a real cross-check.
Sharing the *convention* (`ddof = 1`, the documented edge cases) is not
sharing the *algorithm*: the convention is the definition being compared
against, the algorithm is what is under test.

Both live in `python/common/`, not a new `python/participant/` package: they
are platform-level numeric references, the same category as
`python/common/determinism.py`, not participant production code (C++ owns
that path, ADR-0005), so they need no scope widening in
`configs/milestone_scope.yaml`.

## Alternatives considered

- **Naive running-sum recomputation on eviction** — rejected *for production*:
  not numerically stable under a sliding window, which is exactly what
  AEGIS-099 rules out by name. It is, however, exactly the right choice for
  the offline *reference* (`python/common/offline_stats.py`), where being
  obviously correct against the definition matters more than being fast or
  stable, and where using a different algorithm from production is the whole
  point.
- **NumPy as the Python reference** — rejected: not independent of the same
  floating-point library pitfalls the comparison is meant to catch, and adds a
  dependency for a comparison that stdlib arithmetic already answers.
- **A `python/participant/online_stats.py` package** — rejected: needs a new
  architecture-rules layer and a scope widening neither this slice nor the
  approved plan calls for; the reference is platform-level, not participant
  production code.
- **Accepting participant-domain types for caller convenience** (e.g. a
  `push(TradeEvent)` overload) — rejected: the first such overload is the
  first participant-domain dependency this library was narrowed specifically
  to avoid (ADR-0020).

## Consequences

- AEGIS-101–107 (rolling covariance, correlation, z-score, exponential
  statistics, realized volatility/beta, drawdown, cross-language validation)
  extend `RollingMoments` and its siblings with the same reverse-Welford
  discipline and the same plain-numeric surface; none of that work touches
  this ADR's decisions.
- M4's strategy layer and M6's attribution work call `cpp-statistics`
  directly on values they extract themselves, with no adapter layer needed.

## Verification

- `tests/cpp/unit/test_rolling_moments.cpp` — mean and variance match a
  trusted offline (two-pass) calculation over random and adversarial (large
  common offset, near-zero variance) fixtures within a stated tolerance;
  windows of size 0 and 1 report the documented edge-case values; a
  push/evict sequence longer than the window matches recomputing from the
  current window's contents alone.
- `tests/unit/test_online_stats.py` — the Python reference matches the same
  offline calculation over the same fixtures.

## Owner approval

Authorized under the owner-approved M3 plan of record
(`experiments/plans/M3.md`), 2026-08-12.
