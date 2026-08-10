# ADR-0015: Futures contract identity and lifecycle

- Status: Accepted
- Date: 2026-08-10
- Requirement IDs: AEGIS-011, AEGIS-012
- Milestone: M2

## Context

M2 needs a futures domain to build the rest of the milestone on: normalized
ingestion (AEGIS-026), roll policies (AEGIS-015..018), continuous series
(AEGIS-019..022) and the roll audit (AEGIS-023) all key their output by
contract, and none of them can start until "what identifies one contract"
and "what state is a contract in on a given date" have single, fixed answers.

Two failure modes were the ones to design against. If contract identity has
more than one valid spelling, provenance silently splits: two rows that are
the same contract stop being joinable, and a roll audit under-reports. If
lifecycle is stored as mutable state rather than computed, two components
that check a contract's state on the same date can disagree — which for a
research platform is worse than either being simply wrong, because nothing
signals that they disagree.

M2 slice 1 already introduced `ContractId` (`cpp/replay`'s canonical replay
order needs a `contract_symbol` component, so identity had to exist before
replay did). This ADR is the identity and lifecycle design that slice 1's
identity type was built against, and the lifecycle half slice 2 adds on top
of it. The calendar half — trading-session calendars, holidays, DST — is
AEGIS-013 (M2 slice 3) and is out of scope here.

## Decision

**Identity: `(venue, product_root, year, month)`, one canonical string.**
`ContractId` (`python/futures/identifiers.py`, unchanged from slice 1) is a
frozen, hashable dataclass with exactly one string form,
`VENUE:ROOT:YYYYM` (e.g. `SYNX:EQX:2026Z`), and `parse()` accepts only that
form. A four-digit year, not the one- or two-digit screen form: `Z6` is 2016,
2026 and 2036, and resolving that ambiguity by guessing puts a
decade-long hole in a continuous series that nothing downstream can detect.
Lexicographic order of the canonical string agrees with chronological order
within one product, so a sort of contract-keyed rows stays reproducible
without a bespoke comparator.

**Metadata: product-level defaults, schema-validated.** `Product`
(`python/futures/instruments.py`) carries venue, product root, tick size, lot
size, multiplier, currency, timezone and an opaque session-template
reference, validated against `configs/schemas/futures_product.v1.json` —
the same discipline `python/common/config.py` uses for AEGIS-231: one
schema, a mandatory enumerated version, every problem reported at once
rather than the first. `tick_size` and `multiplier` are parsed from decimal
*strings*, never JSON/YAML numbers, because a float loses exactness on
values like `0.1` and these are the values every price and notional
calculation is a multiple of.

**Lifecycle: a pure function, not stored state.**
`lifecycle_state(contract, as_of) -> ContractLifecycle` in
`python/futures/contracts.py` computes one of five states from the
contract's own dates and an explicit `as_of` date — never a wall clock, never
a field on `Contract` that could fall out of sync with what the dates say:

```
PreListing -> Active -> LastTradingDay -> Expired -> Settled
```

checked as an ordered chain so a boundary where `last_trade_date == expiry`
(cash settlement, the common case) resolves to `LastTradingDay` on that day
rather than skipping to `Settled` — the `Expired` window simply has zero
width for that contract, which is a property of its dates, not a special
case the function has to detect.

**The chain orders; it does not pick a "front" contract.**
`ContractChain` (`python/futures/chain.py`) holds every known contract for
one product in chronological order and answers lookup and "what is listed on
this date" — a lifecycle fact, always zero-or-more contracts. It
deliberately has no method that returns a single "current" contract.
Fixed-days, volume-crossover, open-interest-crossover and liquidity-score
(AEGIS-015..018, M2 slice 6) are four different, mutually incompatible
answers to which contract is front on a date; a chain method picking one by
nearest expiry would silently be a fifth, undocumented roll policy baked
into the identity layer everything else depends on.

## Alternatives considered

- **Two- or one-digit contract years** (`Z6`, `Z26`) — the screen convention,
  rejected: ambiguous across decades, and the ambiguity is exactly the kind
  a research platform must not resolve by guessing.
- **Lifecycle as a stored, mutable field** — rejected: two components
  checking the same contract on the same date could disagree, and nothing
  would signal the disagreement. A pure function of `(contract, as_of)`
  cannot drift from itself.
- **`tick_size`/`multiplier` as JSON/YAML numbers** — rejected: floats lose
  exactness on ordinary decimal values, and these numbers are multiplied
  into every price and notional downstream.
- **A `ContractChain.front(as_of)` method** — rejected: it would have to
  pick a roll rule to implement, and every available rule belongs to M2
  slice 6, not to the identity layer.
- **A fifth or sixth lifecycle state** (e.g. separating "expired, cash
  pending" from "expired, physical pending") — rejected: no frozen AEGIS-012
  acceptance criterion asks for it, and the five approved states already
  distinguish cash and physical settlement through `SettlementType`, not
  through additional lifecycle states.

## Consequences

- Every later M2 slice keys its output by `ContractId.canonical`, with no
  second identity scheme to reconcile.
- `python-futures` still has no edge to `python-data` or `python-common`
  (`configs/architecture_rules.yaml` permits both, but slice 1 and slice 2
  needed neither). The first module that does — ingestion, AEGIS-026, M2
  slice 4 — establishes that edge, not this one.
- The `InstrumentSpec`-shaped projection onto M1's integer tick/lot grid
  (`cpp/exchange/order_book/instrument.hpp`) is not built here: nothing in
  M2 slices 1-2 calls it. Storing `tick_size`/`multiplier` as `Decimal`
  rather than `float` is what keeps that projection lossless whenever a
  later slice needs it.
- Trading-session calendars, holidays and DST remain AEGIS-013 (M2 slice 3).
  `session_template` stays a name until then; nothing in this ADR's code
  interprets it.

## Verification

- `tests/unit/test_futures_instruments.py` — schema validation (missing
  fields, wrong types, unsupported `schema_version`, unknown timezone),
  duplicate-key rejection, and the three committed product families load
  through `load_catalog()`.
- `tests/unit/test_futures_contracts.py` — construction validation
  (`first_trade_date <= last_trade_date <= expiry`), every lifecycle
  boundary date including the `last_trade_date == expiry` case, and that
  `SettlementType`/`ContractLifecycle` carry no state beyond the five and two
  values named above.
- `tests/unit/test_futures_chain.py` — lookup, duplicate-contract rejection,
  cross-product rejection, and `listed_at`.
- `tests/property/test_contract_identity.py` — contract identity round-trips
  through `parse()`/`canonical` for arbitrary valid inputs; lifecycle state
  is monotonic non-decreasing in `lifecycle_index` as `as_of` increases
  across arbitrary date ranges; chain iteration order is invariant to
  insertion order.

## Owner approval

Authorized as part of M2 slice 2 under the owner-approved M2 plan of record
(`experiments/plans/M2.md`, rev. 4), 2026-08-10.
