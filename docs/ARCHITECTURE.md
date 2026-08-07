# AEGIS Architecture

## System boundary

```text
                    AEGIS EXCHANGE
Ingress -> Sequencer -> Matching Policy -> LOB -> Trades/Book Events
                                        |
                                  Event Store/Snapshots
                                        |
          market data / acknowledgements / fills
                                        |
                 AEGIS TRADING PARTICIPANT
Feed Adapter -> Book Builder -> Features -> Strategies -> Risk -> OMS -> Gateway
                                      |                    |
                               Research/Validation     Portfolio/P&L
                                      |
                  Decision Arena / Analytics / Dashboard
```

## Non-negotiable dependency rules

1. Exchange code may not import strategy, participant-risk, dashboard, or research code.
2. Strategy code emits proposals only; it cannot call an exchange or broker adapter.
3. Risk produces approve/resize/reject decisions with reason codes.
4. OMS alone owns participant-side order lifecycle.
5. Portfolio state changes only from accepted execution/account events.
6. Research consumes immutable versioned datasets and experiment manifests.
7. Decision Arena uses frozen-at-decision information and the same risk/execution abstractions.
8. Python orchestrates research and reporting; latency-sensitive deterministic cores live in C++.
9. No shared mutable book across matching threads; one writer per book partition.
   M1 built the single-instrument case (`ExchangeNode`, `cpp/exchange/**`,
   single-threaded); multi-writer sharding across partitions is AEGIS-047 (M8).
10. Every external boundary uses versioned messages and explicit clocks.

## Exchange core (M1)

The identifier spaces, price/quantity grid, documented complexity and
invariant scopes are concrete decisions, not just principles — recorded in
[docs/EXCHANGE_CORE.md](EXCHANGE_CORE.md) and `adr/0009` through `adr/0013`,
not duplicated here.

## Clock model

Use explicit clocks:
- `EventTime`: source/exchange event timestamp.
- `ReceiveTime`: participant receipt timestamp.
- `DecisionTime`: strategy or human action timestamp.
- `SubmitTime`: OMS/gateway submission timestamp.
- `ExchangeTime`: exchange processing timestamp.
- `Ack/FillTime`: acknowledgement/execution timestamp.
- `MonotonicTime`: local latency measurement only.

Never compare wall-clock timestamps for benchmark latency without documenting synchronization.

## Data provenance

Every market observation and derived feature must be traceable to:
- source dataset and version,
- contract identifier,
- roll method when continuous data is used,
- original event timestamp,
- transformation version,
- experiment ID and code commit.

## Runtime modes

The same participant contracts must support:
1. historical deterministic replay,
2. in-process simulated exchange,
3. accelerated interactive Decision Arena,
4. delayed/live paper data and paper broker adapter.

## Primary language boundaries

C++:
- sequencer, LOB, matching, event bus, replay core,
- participant book builder, online statistics, OMS, portfolio, risk,
- latency-sensitive simulation and bindings.

Python:
- ingestion and data quality,
- futures roll research,
- statistical tests and strategy experiments,
- validation, attribution, reporting, orchestration.

UI/application:
- FastAPI or equivalent versioned API,
- React or a deliberately scoped Streamlit prototype,
- no direct engine-memory access from UI.
