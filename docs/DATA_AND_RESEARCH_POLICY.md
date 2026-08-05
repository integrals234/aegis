# Data and Quantitative Research Policy

## Data

- Commit only small, legally redistributable samples.
- Licensed or large datasets remain outside Git and are referenced by immutable version IDs.
- Every transformation records source, timestamp convention, timezone, contract, roll method and schema version.
- Never fabricate historical data to support a CV trading metric.
- Synthetic data is for correctness, stress and known-ground-truth tests; it is not evidence of market profitability.

## Research

- State the economic hypothesis before examining final test performance.
- Correlation alone does not establish a mean-reverting spread.
- Cointegration/stationarity tests have assumptions and are not guarantees.
- All rolling features are timestamped and leakage-tested.
- Hyperparameters are selected without touching the final test set.
- Report gross and net results.
- Include fees, spread, slippage, latency, partial fills and roll effects where relevant.
- Include negative and rejected results.
- Store experiment ID, commit, data version, config, seed, date range, contracts, roll method and costs.

## Required stat-arb workflow

1. Economic rationale and invalidation condition.
2. Contract/data alignment.
3. Hedge-ratio estimation.
4. Residual/spread construction.
5. Stationarity/cointegration analysis where appropriate.
6. Half-life and structural-break analysis.
7. Leakage-free signal generation.
8. Walk-forward validation.
9. Cost-aware execution simulation.
10. Regime and parameter-stability analysis.
11. Portfolio and risk integration.
12. Honest rejection or acceptance report.
