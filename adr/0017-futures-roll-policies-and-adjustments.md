# ADR-0017: Futures roll policies and continuous-series adjustments

- Status: Accepted
- Date: 2026-08-10
- Requirement IDs: AEGIS-015, AEGIS-016, AEGIS-017, AEGIS-018, AEGIS-019, AEGIS-020, AEGIS-021, AEGIS-022
- Milestone: M2

## Context

ADR-0015 deliberately left `ContractChain` with no "front contract" method:
fixed-days, volume-crossover, open-interest-crossover and liquidity-score
are four different, mutually incompatible answers to "which contract is
front on a date", and baking one into the identity layer would silently
make it a fifth, undocumented policy every other one had to work around.
This slice is where that decision actually gets made -- four times, once
per policy -- and each one leaves at least one genuinely underspecified
choice the frozen acceptance text does not resolve: whether "days" in
"fixed-days" means calendar or trading days, what "persistence" means for a
crossover, how missing volume/open-interest should be treated, and how a
composite score should be normalized and weighted. CLAUDE.md's rule for
exactly this situation is to choose the narrowest defensible behavior and
write down why, rather than guess silently -- this ADR is that record for
all four policies at once, because they share one interface and several
design questions.

## Decision

**One shared interface, `RollPolicy.front_contract(chain, observations,
as_of) -> ContractId | None`** (`python/futures/roll/policy.py`), and one
shared observation type, `RollObservation(contract_id, session_date,
volume, open_interest)` -- deliberately narrower than a `futures_bar.v1`
record, so this layer does not inherit ingestion's wider shape for no
reason. Every policy is a frozen, parameter-holding dataclass; none reads a
wall clock or holds mutable state. Every policy chooses only from
`listed_contract_ids_at(chain, as_of)` -- contracts `ContractChain.listed_at`
already says are tradeable that day -- never a contract history alone would
suggest.

**AEGIS-015, fixed-days: calendar days, inclusive boundary, one rule for
every N.** The count is calendar days, not trading/session days -- AEGIS-013's
calendars exist as of M2 slice 3 and could in principle answer "how many
sessions until expiry", but coupling the roll layer to a specific product's
session template would make every roll decision depend on which calendar
that product happens to use, which is a strictly wider dependency than this
policy needs. `days_to_expiry <= days_before_expiry` triggers the roll,
inclusive, with no special case at `N = 0`: it rolls exactly on expiry day
itself, not the day after, because the boundary rule is applied uniformly
rather than case-split per value of `N`.

**AEGIS-016/017, crossover: persistence over *comparable* sessions, missing
never zero, reversal resets.** `crossover_confirmed` walks backward from
`as_of` over sessions where *both* the front and the candidate deferred
contract have a non-null value for the metric being compared -- a session
where either side is missing the metric is skipped entirely, neither
extending nor breaking the streak. A session that fails the crossover
condition breaks the streak immediately (a reversal resets, it does not
pause), so the streak counted is always the most recent unbroken run ending
at or before `as_of`. Volume and open-interest crossover share this exact
mechanism, applied to different metrics, and never substitute one metric
for the other's absence -- `OpenInterestCrossoverPolicy` never falls back to
volume when open interest is missing.

A chain listing more than two contracts is walked pairwise, current front
against the immediately next deferred contract only, one decision at a
time -- never a jump of two contracts in one call, which would conflate two
independent crossover decisions into one.

**AEGIS-018, liquidity score: `Decimal` normalized-share weighting, tie
broken by chronology.** `score = volume_weight * (volume / total listed
volume) + open_interest_weight * (open_interest / total listed open
interest)`, using each contract's most recent non-null observation on or
before `as_of`. Weights and every intermediate component are `Decimal`,
matching the M2 slice 2 precedent for `tick_size`/`multiplier` -- a `float`
composite score would make the *selection itself*, not just a downstream
notional, potentially irreproducible across platforms. A missing metric
contributes exactly zero to its component rather than raising, since the
score is a relative-share measure and a contract with no observed volume
genuinely has no volume share to claim. Ties are broken by
`listed_contract_ids_at`'s own chronological order: Python's `max()` returns
the first maximal element of an already-ordered sequence, so no separate
tie-break rule is written or needed.

## Alternatives considered

- **Trading/session-day counting for AEGIS-015** -- rejected: it would
  couple the roll layer to a specific calendar template, a dependency this
  policy does not otherwise need; see Decision.
- **Treating missing volume/open-interest as zero** -- rejected explicitly
  by the M2 plan of record ("do not silently treat missing volume as zero
  unless explicitly specified"), and it would make a data outage look like
  a genuine liquidity collapse.
- **A running/non-resetting persistence counter** (crediting the count so
  far even across a reversal) -- rejected: "persistence" describes an
  unbroken run, and crediting a broken one would let a single-day spike
  followed by reversion still eventually trigger a roll it never actually
  earned.
- **Substituting volume for missing open interest, or vice versa, in the
  crossover policies** -- rejected: the two metrics measure different
  things, and conflating them under a data gap would silently change what
  the policy claims to be measuring.
- **A `float` liquidity score** -- rejected for the same reason `tick_size`
  is `Decimal`: the composite score is not just reported, it decides which
  contract is selected, and a platform-dependent float result would make
  that selection irreproducible.
- **Asserting economically-loaded property-test invariants** (e.g. "the
  selected contract's volume is always non-decreasing") -- rejected per the
  M2 plan of record's explicit warning against economically false
  monotonic properties; `tests/property/test_roll_invariants.py` checks
  only reproducibility, chain-validity and order-independence.

## Consequences

- `ContractChain` remains completely unmodified by this slice -- every
  policy consumes its existing `listed_at`/`lookup` surface, confirming
  ADR-0015's bet that identity would not need to change to support roll
  policies later.
- The four policies are independent and swappable: a caller picks one by
  passing a different `RollPolicy` instance, never by branching on a policy
  name inside shared code.
- M2 slice 7's continuous series is built on top of whichever policy a
  caller selects, via the same `front_contract` interface -- it does not
  special-case any one policy.
- `LiquidityScorePolicy.score_breakdown` is public and returns every
  component, not just the winning contract, specifically so a later roll
  audit (AEGIS-023, M2 slice 8) and this slice's own evidence generator can
  show the full basis for a selection, not merely assert one.

## Verification

- `tests/unit/test_roll_fixed_days.py` -- golden roll dates against the
  real committed EQX chain, the inclusive boundary (including the `N = 0`
  edge case), no-listed-contract, and that the policy ignores its
  `observations` argument entirely.
- `tests/unit/test_roll_volume_crossover.py` /
  `test_roll_oi_crossover.py` -- crossover, sub-threshold (not yet
  persisted), reversal-resets-the-count, missing-metric-is-skipped on
  either side, single-listed-contract, and (open-interest only) that volume
  never substitutes for missing open interest.
- `tests/unit/test_roll_liquidity_score.py` -- weight validation, score
  reproducibility, breakdown components summing to the score, missing
  metric contributing zero, and the most-recent-on-or-before-`as_of`
  observation rule.
- `tests/property/test_roll_invariants.py` -- reproducibility, "the
  selected contract is always genuinely listed", and order-independence,
  across all four policies together.
- `experiments/evidence/AEGIS-01{5,6,7,8}/*.json` -- each generated by
  calling the real production policy against the committed EQX chain and a
  documented, fixed synthetic observation series.

## Slice 7 addendum: continuous series and adjustment methods (AEGIS-019..022)

### Context

Slice 6 built four ways to pick a front contract on a date; this slice
consumes a fixed sequence of those picks to build a continuous price series
and two back-adjustment conventions over it. AEGIS-020/021's frozen
acceptance both name a "return/price-change preservation property" without
saying precisely which one, and getting this wrong is easy: the most
naive back-adjustment formula (using only the two *front* prices adjacent
to a roll) produces a series that is perfectly continuous in level but
silently invents a zero return on every roll day, which is a real, if
subtle, fabrication of market history -- exactly the class of thing
CLAUDE.md's "never fabricate data" rule and DATA_AND_RESEARCH_POLICY exist
to prevent, even though nothing about it looks like fabrication at a
glance.

### Decision

**Explicit roll selections in, not a roll policy call.** `series.py`
takes a caller-supplied `Mapping[date, ContractId]` naming the front
contract per date -- it does not import `futures.roll` or call a
`RollPolicy` itself. Roll *selection* (slice 6) and roll *consumption*
(this slice) stay separate: a roll audit (a later slice) can replay a fixed
selection sequence exactly, and series construction is testable without
re-running policy logic for every case.

**Same-day dual quote at every roll -- the only convention that actually
reconciles returns.** At a roll from old contract to new, both
`build_additive_adjusted_series` and `build_ratio_adjusted_series` require
the *old* contract's own price *on the roll date itself* (not the day
before) to compute the gap/ratio. This is what makes AEGIS-022's
reconciliation property hold exactly, proven algebraically and by test:
**the adjusted return across a roll boundary equals the outgoing
contract's own realized return for that specific day** -- not zero, and
not the incoming contract's return. A same-day dual quote is not an extra
burden invented for this ADR's convenience: real futures markets always
trade the outgoing and incoming contracts simultaneously around a roll
(that overlap is what makes a roll possible at all), so the data this
convention needs is data that genuinely exists.

The rejected alternative -- gap computed from the two adjacent *front*
prices only (old contract's day-before price, new contract's roll-day
price) -- makes the series perfectly continuous at the splice (zero jump
in level), which sounds like the more conservative choice. It is not: it
replaces the real, realized price movement of the outgoing contract on the
roll day with an invented zero, which is the actual "artificial roll jump"
AEGIS-022 asks to avoid. Preserving the real return, even though it makes
the level itself discontinuous by that amount, is what "avoiding" an
artificial jump means under this ADR's reading.

**`Decimal` throughout; missing/invalid roll data raises, never guesses.**
Matching the slice 2 and slice 6 precedent: an adjustment factor or offset
changes every downstream price, so it must be exactly reproducible. A
missing same-day dual quote raises `InvalidAdjustment` (additive) or the
same (ratio); a non-positive price where a ratio needs one raises rather
than dividing by zero or substituting a value. `build_unadjusted_series`
raises `MissingPrice` (a distinct, narrower exception) when a selected
front contract has no price at all on a date it was selected for -- a
different failure than a missing roll-date dual quote, so it is not
conflated with `InvalidAdjustment`.

**No synthetic index.** The M2 plan of record marks it optional, and no
AEGIS-019..022 acceptance criterion names one; building it would be
unrequested scope.

**`build_return_stream` is the ratio convention's natural return
measure.** Its output is the simple (proportional) return
`adjusted[t]/adjusted[t-1] - 1`, which is exactly comparable across the
ratio-adjusted series. The additive convention's own reconciliation
property is a price *difference*, not a proportional return, and is
checked directly against `adjusted_price` deltas
(`tests/unit/test_series_reconciliation.py`) rather than forced through
the same function.

### Alternatives considered

- **Adjacent-front-price gap (forced continuity)** -- rejected; see
  Decision. This was the first design considered and was caught by the
  reconciliation test itself failing against a hand-derived expectation,
  which is exactly the kind of thing a "known roll-gap fixture" is for.
- **Calling a `RollPolicy` from inside `series.py`** -- rejected: it would
  couple series construction to one specific selection mechanism and make
  it untestable independent of policy logic; see Decision.
- **A single "return" abstraction for both additive and ratio** --
  rejected: the two conventions preserve different quantities (absolute
  difference vs. proportional return), and forcing one function to serve
  both would obscure which one a given return value actually means.
- **A synthetic index** -- rejected as out of scope; see Decision.
- **Silently treating a missing roll-date dual quote as "no adjustment
  needed"** -- rejected: it would silently under-adjust every earlier
  observation, which is a wrong answer presented as a complete one.

### Consequences

- The M2 plan of record's canonical replay order and any later roll-audit
  report (AEGIS-023, M2 slice 8) can rely on every continuous-series stage
  -- unadjusted, additive, ratio, return stream -- carrying `contract_id`
  provenance on every observation, with no stage that erases it.
- A caller wanting a different adjustment convention (e.g. adjacent-front
  continuity, for some other legitimate purpose) would need a new function
  in this module, not a parameter to the existing ones -- the two
  documented conventions are not configurable variants of one algorithm,
  they are different algorithms with different reconciliation properties.

### Verification

- `tests/unit/test_series_unadjusted.py` -- provenance, `is_roll_point`
  boundaries, sort-order independence from mapping order, `MissingPrice`
  on a selected front contract with no price, and that a price for a
  non-front contract never leaks into the series.
- `tests/unit/test_series_additive.py` / `test_series_ratio.py` -- a known
  roll-gap golden fixture with the documented offset/ratio, multiple
  sequential rolls each applying their own gap/ratio, a genuine cross-year
  roll, and `InvalidAdjustment` on a missing/non-positive roll-date price.
- `tests/unit/test_series_reconciliation.py` -- the reconciliation property
  proven across *every* roll in a multi-roll fixture at once (not one
  hand-picked day), for both conventions, plus end-to-end provenance
  through every stage including the return stream.
- `tests/property/test_adjustment_invariants.py` -- determinism,
  independence from the `prices` list's insertion order, and provenance
  presence, across arbitrary valid roll schedules (up to two rolls, three
  contracts, 2-9 days).
- `experiments/evidence/AEGIS-01{9},02{0,1,2}/continuous_series_and_adjustments.json`
  -- a genuine cross-year, two-roll fixture built through the real
  production pipeline, with both reconciliation checks verified
  programmatically (the generator raises rather than writing an
  unsupported claim).

## Owner approval

Authorized as part of M2 slice 6 (roll policies, AEGIS-015..018) and M2
slice 7 (this addendum: continuous series and adjustments, AEGIS-019..022),
both under the owner-approved M2 plan of record (`experiments/plans/M2.md`,
rev. 4) and the owner's slice 3-7 continuous-execution prompt, 2026-08-10.
