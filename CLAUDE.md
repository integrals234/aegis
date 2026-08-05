# AEGIS Claude Code Operating Contract

You are building AEGIS, a correctness-first multi-market futures research, exchange-simulation, risk, execution and trader-decision-intelligence platform.

## Read first in every session

1. `docs/BUILD_STATE.md`
2. `docs/MASTER_SPEC.md`
3. `requirements/requirements.json`
4. `requirements/implementation_status.json`
5. `docs/ARCHITECTURE.md`
6. `docs/ROADMAP.md`
7. the active milestone prompt under `prompts/`

## Frozen files

Never edit:
- `docs/MASTER_SPEC.md`
- `requirements/requirements.json`
- `docs/BENCHMARK_POLICY.md`
- `docs/CV_CLAIMS_POLICY.md`
- `docs/DATA_AND_RESEARCH_POLICY.md`

The owner may edit them manually. The hook intentionally blocks your edits.

## Non-negotiable behavior

- Do not implement before producing a requirement-ID-mapped plan in plan mode.
- Work on exactly one active milestone.
- Do not delete, weaken, rename, merge away, or silently defer requirements.
- Do not mark a requirement complete because code compiles.
- `verified` requires existing evidence paths and an independent spec-auditor review.
- No TODO, stub, fake service, generated screenshot, or hardcoded demo output counts as completion.
- Never fabricate data, benchmarks, test output, trading returns, or CV metrics.
- Never optimize before deterministic correctness and baseline tests.
- Never let strategy or UI code bypass independent risk and OMS.
- Keep exchange and participant boundaries strict.
- Prefer simple, testable architecture over buzzwords.
- Record nontrivial architecture decisions in `adr/`.
- Use stable IDs and explicit clocks; avoid hidden global state.
- Run the smallest relevant tests after edits, then the full milestone gate before closure.
- Update `requirements/implementation_status.json` only with truthful paths that exist.
- Keep commits small and reference requirement IDs.
- If the specification conflicts with implementation convenience, the specification wins.
- If a requirement is ambiguous, choose the narrowest defensible behavior, document it in an ADR, and do not invent market facts.

## Completion response for every implementation task

Return:
1. requirement IDs addressed;
2. files changed;
3. tests/commands executed with results;
4. evidence paths added;
5. limitations/blockers;
6. next smallest task;
7. confirmation that frozen files were not modified.

## Architecture laws

- Exchange: sequencing, LOB, matching, exchange events.
- Participant: feed handler, local book, features, strategies, risk, OMS, portfolio.
- Strategies emit proposals only.
- Risk returns approve/resize/reject.
- OMS owns order lifecycle.
- Portfolio changes from accepted execution/account events.
- Python is for research/orchestration/reporting; C++ owns deterministic and latency-sensitive cores.
- One writer per order-book partition.
- Runtime boundaries use versioned messages.

## Quality order

Correctness → determinism → reproducibility → risk safety → research validity → observability → performance → UI polish.
