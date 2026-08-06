# Limitations

What AEGIS does not do, cannot currently claim, or enforces only partially. This
document exists because the alternative to writing limitations down is having
them inferred incorrectly from what the repository appears to contain.

Read it alongside [docs/DEFERRED_VERIFICATION.md](DEFERRED_VERIFICATION.md),
which lists the requirements whose verification is registered as outstanding.

## What exists at M0

M0 is the governance and engineering foundation. **There is no exchange, no
order book, no matching engine, no strategy, no risk engine, no OMS, no
portfolio, no research, no attribution, no Decision Arena and no dashboard.**
`docs/ROADMAP.md` places each of those in M1 through M9.

What exists is: governance tooling with tests, a C++20 and Python toolchain, a
layer-dependency checker, clock domains, a configuration system, structured
logging, a metrics registry, a message envelope with canonical encoding, a
determinism harness, minimal bindings, a schema registry, an experiment-manifest
schema, and CI.

## Claims that cannot be made yet

| Claim | Why not |
|---|---|
| "AEGIS is deterministic" | M0 shows only that **the harness detects nondeterminism**. There is no engine whose determinism could be tested; the producers emit platform records. |
| Any latency or throughput figure | Nothing has been benchmarked. `docs/BENCHMARK_POLICY.md` governs when a figure may be quoted, and M8 owns the work. |
| Any complexity claim (`O(1)` lookup, `O(k)` matching) | Those are AEGIS-036 and AEGIS-039, unimplemented. |
| Any trading result, Sharpe or drawdown | No strategy, no data, no backtest. |
| "Production", "HFT", "live" or "institutional-grade" | Simulation-only code; `docs/CV_CLAIMS_POLICY.md` forbids the phrasing, and `tools/check_claims.py` enforces it. |
| Test coverage percentage | No coverage gate is configured; no M0 requirement asks for one. |

## Controls that are partial by construction

### The specification hook is defence in depth

`.claude/hooks/protect_spec.py` sees only writes made through Claude's file
tools. **A shell redirect, `sed -i`, an editor, or `python -c` bypasses it
entirely**, and no amount of hardening changes that — the interception surface is
unbounded.

It has been hardened to fail closed on unparseable input, non-object payloads and
a missing project directory, and extended to cover
`requirements/frozen_hashes.json`. It is still not the control of record. The
authoritative controls are `tools/check_frozen.py` (hash **and** branch diff),
the pre-commit hook, and CI. See [adr/0006](../adr/0006-governance-evidence-milestone-control.md).

### Settings deny rules are a configuration assertion

`tests/unit/test_settings_deny_rules.py` asserts that `.claude/settings.json`
declares deny rules for `.env`, secrets, credentials and key material. Whether
the harness honours them is a property of Claude Code, not of this repository.
The behavioural control for committed secrets is `tools/scan_secrets.py`, which
scans the worktree, the index and git history.

### The mutable-globals check is a heuristic

`tools/check_architecture.py` detects file-scope mutable definitions by text, not
by parsing C++. It over-reports rather than under-reports. A false positive costs
a review comment; hidden global state costs a replay nondeterminism investigation
several milestones later.

### The C++ JSON Schema validator implements a subset

`cpp/common/config.cpp` supports `type` (including unions), `required`,
`additionalProperties` (boolean or schema), `enum`, `minimum`/`maximum`,
`minLength`/`maxLength` and `pattern`. Any other keyword is **reported as
unsupported** rather than ignored, because a validator that skips what it does
not understand accepts documents the schema author meant to reject. The Python
loader uses full JSON Schema draft 2020-12.

## Environment limitations

### No clean-machine transcript

`docker/Dockerfile.dev` describes the reproducible environment but **has not been
built**: Docker Desktop WSL integration is disabled on the M0 development host.
M0 therefore closes on local-virtualenv evidence, and AEGIS-009 carries a
registered obligation for a transcript from a container or CI runner.

### No CI run has happened

No git remote is configured and no workflow has executed. `.github/workflows/ci.yml`
is written and `scripts/ci_local.sh` runs the same stages locally, but AEGIS-234's
acceptance names *a passing workflow on the protected default branch*. Creating
the remote and the branch-protection rule are owner actions. AEGIS-227, AEGIS-233
and AEGIS-234 carry obligations for the same reason.

### Development happens under WSL2

`tools/capture_environment.py` detects this and records
`virtualisation.bare_metal_claimable: false`. Per `docs/BENCHMARK_POLICY.md`
rule 2, any figure measured here must be labelled a WSL figure. Serious latency
work belongs on native Linux.

## Scope decisions recorded in ADRs

- **No price type yet.** Fixed-point scale, tick handling and rounding are
  decided in M1 with the order book that uses them ([adr/0002](../adr/0002-time-clocks-and-envelope.md)).
- **No columnar interchange yet.** Parquet/Arrow/DuckDB arrive with M2's data;
  building them now would require inventing the futures schema AEGIS-026 owns
  ([adr/0007](../adr/0007-data-interchange-and-experiment-manifest.md)).
- **No snapshot store.** Each module will own its persistence; a shared store is
  where the exchange/participant boundary would quietly dissolve
  ([adr/0008](../adr/0008-snapshot-and-recovery-contract.md)).
- **No domain metrics.** Queue depth, execution latency and risk status appear
  with their producers in M1, M3 and M5. A gauge nobody writes reads zero, and an
  operator believes the zero ([adr/0004](../adr/0004-config-logging-observability.md)).
- **C++20, not C++23.** Changing the language standard narrows the set of
  toolchains that can build AEGIS and is its own decision
  ([adr/0005](../adr/0005-toolchain-and-language-boundary.md)).

## Data limitations

The only committed sample is synthetic, generated by `tools/make_sample_data.py`.
It describes no real instrument, venue or trading day, and no conclusion about
any market may be drawn from it. `configs/external_datasets.yaml` is empty: no
research has run, so registering a vendor feed would record an intention rather
than a fact.
