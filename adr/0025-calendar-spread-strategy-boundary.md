# ADR-0025: Calendar-spread strategy boundary and market-data construction

- Status: Accepted
- Date: 2026-08-17
- Requirement IDs: AEGIS-004, AEGIS-076, AEGIS-077, AEGIS-078, AEGIS-080
- Milestone: M4

## Context

M4 populates `cpp/participant/strategy`, empty since M0 and dated M4 in
`configs/architecture_rules.yaml`, with the platform's first strategy. Two
decisions have to be made before any code is written: exactly where a
strategy's authority ends and the existing risk/OMS seam begins (the thing
AEGIS-004's residual has been waiting on since M0), and what market data the
strategy runs against, given `data_samples/futures/` carries only one
contract's daily settlement-bar history per product -- not the two
simultaneous contract price series a calendar spread needs.

## Decision

**The strategy emits proposals only.** `CalendarSpreadStrategy::on_book_update`
(`cpp/participant/strategy/calendar_spread_strategy.hpp`) takes two
`book::TopOfBook` values (the near and far leg's reconstructed state) and
returns a `StrategyProposal` -- two `StrategyLeg { instrument_id, side,
quantity_units }` values and nothing else. No order id, no client-order-id,
no `OrderManager` call, no exchange or gateway header anywhere in
`cpp/participant/strategy`. This is mechanical, not just documented: the
layer's `may_depend_on` in `configs/architecture_rules.yaml` is
`[cpp-common, cpp-events, cpp-participant-book-builder, cpp-statistics]` --
it cannot reach `cpp-participant-oms` or any `cpp-exchange-*` layer even if a
future change tried to make it.

**The participant composition root is the only place a proposal is turned
into an order.** `cpp/participant/app` (`m4-architecture-transition`, PR #10)
gains exactly one edge -- `cpp-participant-app.may_depend_on +=
cpp-participant-strategy` -- and `run_calendar_spread_scenario`
(`cpp/participant/app/participant_run.cpp`) is the one function that reads a
`StrategyProposal` and calls `OrderManager::submit_new_order` for each leg.
This mirrors exactly how `run_participant_fixture` already owns the only
OMS/portfolio wiring in that file.

**The mandatory risk seam is unchanged and still a test/fixture double.**
Every order a proposal generates passes through the same `RiskGate` seam
`OrderManager::submit_new_order` has enforced since M3 (AEGIS-108); M4 ships
no production `RiskGate` implementation, reusing the exact pattern
`run_participant_fixture`'s `AlwaysApproveRiskGate` already established
(ADR-0023). No M5 risk policy is implemented or anticipated by this decision.

**Execution has two legitimate paths, never conflated.** The production CLI
path (`aegis_participant_run --calendar-spread`) uses a new
`ImmediateFillExecutionAdapter`: no transport, no exchange (AEGIS-119) --
`submit`/`cancel`/`modify` only record that a call was made, and the
composition root synthesizes the resulting accept/trade/terminate sequence
itself, deterministically, from the run's own strategy-generated intent. The
real-matching proof (`tests/cpp/unit/
test_calendar_spread_exchange_integration.cpp`) instead reuses
`InProcessExchangeTransport` (extracted to
`tests/cpp/support/in_process_exchange_transport.hpp` from
`test_participant_exchange_integration.cpp` in this same change, so there is
still exactly one implementation of it) against a real, unmodified M1
`ExchangeNode`. Both live entirely on their own side of the boundary: the
first never sees `cpp-exchange-*` at all; the second is composed in `tests/`,
outside `covered_roots`, so no production participant-> exchange edge is ever
created by either.

**The market-data stream this strategy runs against is a documented
construction, not observed tick data.** `data_samples/futures/bars/eqx.csv`
carries six daily settlement bars for one contract (`SYNX:EQX:2026H`);
no second contract's price history is committed anywhere in the repository.
`python/research/calendar_spread.ConstructedBasisRule`
(`tools/generate_calendar_spread_stream.py`) derives the far leg's price
from the near leg's own observed price via a fixed, documented, per-index
basis sequence, and every `CalendarSpreadObservation` this module returns
names its actual source explicitly (`far_price_provenance`,
`far_price_observed`). The near leg's identity and price are **observed
within the committed series**, and the far leg's identity is likewise a real
entry in that `ContractChain`; only the far leg's *price* is constructed.

**"Observed" here never means "real market data."** Every bar in
`data_samples/` is synthetic sample data on the fictional venue `SYNX`
(`data_samples/PROVENANCE.yaml`). "Observed" distinguishes a value that was
read from the committed series from one this code synthesized — nothing more.
No AEGIS-076..081 figure describes a real product, venue or price. `python/research/stream_builder.py` turns that into the
committed two-leg JSONL fixture
(`tests/unit/fixtures/participant/calendar_spread_stream.jsonl`) both demo
paths replay. No execution-quality, fill-realism, or backtest-return claim
rests on this data; none is made anywhere in M4's evidence.

## Alternatives considered

- **Let the strategy call `OrderManager` directly** -- rejected: this is
  exactly the production participant -> exchange-adjacent shortcut
  `configs/architecture_rules.yaml`'s forward-only pipeline comment already
  forbids, and it is what AEGIS-004's residual has been waiting to prove does
  *not* happen once real strategy code exists.
- **A second, independent `InProcessExchangeTransport`** for this test file
  -- rejected: two copies of the same test-only exchange bridge could drift,
  and the existing file's own header claims it is "the only code anywhere
  that does this." Extracting it to `tests/cpp/support/` keeps that claim
  true instead of quietly making it false.
- **Wait for real second-contract market data before building the strategy**
  -- rejected: no frozen M4 requirement asks for intraday or multi-contract
  tick data, and the plan of record prioritizes reaching a real
  signal -> order -> fill -> P&L path within Batch 1. A documented synthetic
  construction, honestly labeled, is the narrower and faster defensible
  choice; real intraday data would be a scope change requiring the owner's
  decision.
- **A production `RiskGate` implementation now, ahead of M5** -- rejected:
  out of M4's scope entirely (`configs/architecture_rules.yaml` dates
  `cpp-participant-risk` to M5); reusing the existing test/fixture double is
  the only legitimate M4 choice.

## Consequences

- M5's risk engine, when it arrives, plugs into the same seam
  `CalendarSpreadStrategy`'s proposals already pass through -- no rewiring of
  the strategy or composition root needed.
- Any future strategy (M6) follows the same proposal-only shape and the same
  composition-root wiring point.
- `tests/cpp/support/in_process_exchange_transport.hpp` is now the shared
  real-exchange test harness for every participant<->exchange integration
  test, present and future.
- Any report or evidence built on the demo fixture must carry ADR-0025's
  construction disclosure forward; it must never be cited as observed
  execution quality.

## Verification

- `tests/cpp/unit/test_calendar_spread_strategy.cpp` -- the strategy never
  calls anything beyond `book::TopOfBook` and `stats::RollingZScore`;
  leakage-free scoring; entry/exit thresholds.
- `tests/cpp/unit/test_calendar_spread_exchange_integration.cpp` -- the
  proposal reaches a real, unmodified M1 `ExchangeNode`, real FIFO matching
  produces fills, `OrderManager`/`Portfolio` reconcile, two independent runs
  are identical.
- `tools/check_architecture.py` -- `cpp-participant-strategy`'s
  `may_depend_on` has no OMS/exchange/gateway edge; no production
  `cpp-participant-strategy -> cpp-exchange-*` edge exists anywhere.
- `aegis_participant_run --calendar-spread --stream
  tests/unit/fixtures/participant/calendar_spread_stream.jsonl` -- two
  independent invocations produce byte-identical output.

## Owner approval

Authorized under the owner-approved M4 plan of record, activated by PR #10
(`configs/governance/policy.yaml`'s `m4-architecture-transition` approval),
2026-08-17.
