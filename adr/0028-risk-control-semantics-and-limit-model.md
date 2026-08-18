# ADR-0028: Risk control semantics and limit model

- Status: Accepted
- Date: 2026-08-18
- Requirement IDs: AEGIS-121, AEGIS-122, AEGIS-123, AEGIS-124, AEGIS-125,
  AEGIS-126, AEGIS-127, AEGIS-128, AEGIS-129, AEGIS-130, AEGIS-131,
  AEGIS-132, AEGIS-133, AEGIS-134, AEGIS-135, AEGIS-136
- Milestone: M5

## Context

Each M5 risk requirement names a control by title and a one-line acceptance
(`requirements/requirements.json`); none specifies an exact formula, unit
convention or config shape. Left unstated, the risk of implementation
convenience overriding conservative defaults is real -- this ADR records the
narrowest defensible reading chosen for each control, so a later reader (or
auditor) can judge the choice against the frozen text rather than the code
alone.

## Decisions, by control

**AEGIS-122 position limits: projected exposure includes reservations.** A
projected position is `confirmed position (RiskState::apply_fill) + every
outstanding reservation for that instrument + this candidate`. Without the
reservation term, two orders that each individually pass could jointly
breach the limit before either fills -- proven as a test
(`RiskEnginePositionLimits.TwoIndividuallyAcceptableOrdersJointlyBreach`).
Reservations are created at `decide_order` (once a `client_order_id` exists)
and released on fill-completion, cancel, downstream rejection, or the
explicit `release_reservation` primitive (ADR-0027's documented backpressure
gap).

**AEGIS-123 notional and currency: single documented base, explicit FX,
explicit unsupported-currency rejection.** Every in-repo product
(`configs/futures/products.yaml`) is `currency: USD`, so `base_currency:
"USD"` with an empty `fx_rate_to_base` map is the honest default -- no real
multi-currency market data exists to validate a conversion against
(`docs/LIMITATIONS.md`). The mechanism itself is real: an instrument whose
`InstrumentInfo::currency` has no `fx_rate_to_base` entry is rejected
`kUnsupportedCurrency`, never silently treated as 1:1 with base. This is the
frozen acceptance's explicit "or unsupported scope is explicit" branch, paid
honestly rather than by pretending multi-currency coverage that was never
tested against real data.

**AEGIS-125 price collars: reference price never moves on a stale or
invalid update.** `RiskState::note_market_quote` only overwrites
`last_valid_quote_` when `MarketQuote::valid` is true; an invalid or
(judged, not enforced, by this method) stale update is still recorded as
"last observed" for staleness purposes but never becomes the collar's
reference. This is what stops a manipulated or corrupted single tick from
resetting the band a subsequent legitimate order is judged against.

**AEGIS-126 staleness and validity are two different rejections, and
validity is decided once, at ingestion.** `on_market_data(..., valid)`
receives validity as an explicit parameter (crossed book, non-positive
price -- structural facts the composition root, not the risk engine, is
positioned to detect from the raw book). Staleness (`kStaleMarketData`) is
judged per-request, against the injected clock, from the last VALID quote's
own timestamp -- so an instrument that has had no valid update in
`max_quote_age` rejects even if the most recent (invalid) update was just
now.

**AEGIS-127 idempotency key, and no cross-process persistence claim.** The
key is `strategy_id | proposal_id | leg_index` -- the identifiers the
composition root already assigns deterministically per proposal. Frozen
acceptance requires rejecting a duplicate or replayed submission during the
engine's own lifetime; it does not require surviving a process restart, and
this implementation makes no such claim: `RiskState::dedupe_keys_` is
in-memory only. A future milestone that needs cross-process dedupe recovery
should compose it with the existing snapshot/recovery contract
(`docs/RECOVERY_CONTRACT.md`), not invent a second persistence mechanism
inside the risk engine.

**AEGIS-128 rate limits cover both orders and cancels, and safety cancels
are exempt by construction.** `RiskEngine::allow_cancel(now, bypass_safety)`
is a **separate** entry point from the order path, called by the composition
root before issuing a kill-switch or connectivity-loss cancel with
`bypass_safety = true` -- which short-circuits before consulting the
rate-limit window at all. This is the one place in the engine where a caller
can bypass a control by construction, and it exists for exactly one reason:
a client message-rate budget must never be able to block the system's own
ability to flatten a position it is trying to protect.

**AEGIS-129 margin: Model A, `margin_per_contract_units * abs(quantity)`.**
Deliberately the dimensionally unambiguous choice: `margin_per_contract_units`
is currency per contract, entering the formula with no multiplier or
reference price to get wrong. This is **not** SPAN, not an exchange clearing
model, and not a claim of production margin adequacy --
`docs/LIMITATIONS.md` states this explicitly, and `RiskLimitsConfig::margin`
carries no field that could be mistaken for one.

**AEGIS-130 leverage: `gross_notional / equity`, non-positive equity
supports no leverage.** `equity_units <= 0` rejects any order whose gross
portfolio notional would be nonzero, rather than dividing by a non-positive
number or silently permitting unlimited exposure -- proven as a test
(`RiskEngineMarginAndLeverage.RejectsExcessiveLeverage`).

**AEGIS-131/132 daily loss and drawdown: latched, exactly-once trip
events.** `RiskState::trip_daily_loss`/`trip_drawdown` return `true` only on
the transition into the tripped state; a caller that observes a further
breach while already tripped gets `false` and emits no second event. Daily
loss resets on `start_new_session` (a new trading day); drawdown does NOT --
a high-water-mark quantity has no session boundary by definition, so a
recovery past the old peak does not un-trip a drawdown latch that already
fired.

**AEGIS-133 volatility-triggered sizing: resize below a hard multiple,
reject beyond it.** `realized_volatility() > target_volatility` scales the
approved quantity by `target_volatility / realized`, floored at 1 unit
(never zero -- a defined minimum, not a silent no-op). At or beyond
`hard_reject_multiple * target_volatility` the order is rejected outright
rather than resized to a token size that would misrepresent what was
actually approved.

**AEGIS-134 concentration and correlation: config-supplied groups, never
estimated online.** `ConcentrationConfig::correlated_groups` is a
caller-supplied instrument grouping standing in for a correlation matrix.
Estimating correlation inside the decision path was considered and rejected:
it would make a risk decision depend on a statistic computed from the same
data stream the decision is about, an unreviewable feedback loop with no
independent check on it. A future milestone wanting genuine online
correlation estimation should build and validate the estimator in
`python/research` or `cpp/statistics` first, then feed its output into this
config as a periodically-refreshed input -- never compute it inline here.

**AEGIS-135/136 kill switches and connectivity: symmetric idempotent
latches, three independent connectivity flags.** Strategy-level and global
kill switches use the same `trip_*`-returns-`true`-once-only pattern as
daily-loss/drawdown. Connectivity is three independent booleans (feed,
exchange, broker) in `RiskState`, each toggled by its own
`on_*_disconnected`/`on_*_reconnected` pair -- a feed outage does not imply
an exchange outage, and the composition root can drive each from its own M2
fault-injection signal.

## Alternatives considered

- **Per-order (not per-proposal-then-per-order) margin/leverage/notional
  checks with no cross-leg accounting.** Rejected: cannot express the
  atomicity ADR-0027 requires for a multi-leg proposal.
- **Estimating FX rates or correlations from available data instead of
  requiring config.** Rejected for both: no real multi-currency or
  inter-instrument correlation data exists in this repository to estimate
  from honestly (`docs/DATA_AND_RESEARCH_POLICY.md`).

## Consequences

- The margin/leverage/notional/concentration path requires the composition
  root to keep `RiskEngine::on_market_data`/`on_equity_update` current; an
  engine that is never fed equity rejects every leveraged order by
  construction (equity defaults to 0), which is the safe failure direction.
- `docs/LIMITATIONS.md` carries an M5 section naming every simplification
  above explicitly, so no downstream reader mistakes this for a production
  risk system.

## Verification

`tests/cpp/unit/test_risk_engine.cpp` (one or more tests per control above),
`tests/cpp/unit/test_calendar_spread_risk_exchange_integration.cpp` (real
exchange, allow/reject/resize/halt), `tests/cpp/unit/
test_risk_fault_market_stress.cpp` (AEGIS-062) and `test_risk_fault_
execution_stress.cpp` (AEGIS-063).

## Owner approval

Implied by merged `m5-architecture-transition`/`m5-participant-app-integration`
(PR #13's activation policy); this ADR is filed alongside the implementation
it documents.
