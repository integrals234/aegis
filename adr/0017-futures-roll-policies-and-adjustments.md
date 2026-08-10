# ADR-0017: Futures roll policies and continuous-series adjustments

- Status: Accepted
- Date: 2026-08-10
- Requirement IDs: AEGIS-015, AEGIS-016, AEGIS-017, AEGIS-018
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

## Owner approval

Authorized as part of M2 slice 6 under the owner-approved M2 plan of record
(`experiments/plans/M2.md`, rev. 4) and the owner's slice 3-7
continuous-execution prompt, 2026-08-10.
