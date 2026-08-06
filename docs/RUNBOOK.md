# Runbook

Operating procedures for AEGIS at M0. Everything here is a command you can run;
where a procedure has a rule, the rule is enforced by a tool named alongside it.

Setup lives in [docs/ENVIRONMENT.md](ENVIRONMENT.md).

## The gate matrix

```bash
export PATH="$PWD/.venv/bin:$PATH"
bash scripts/ci_local.sh
```

Runs every stage `.github/workflows/ci.yml` runs, reports all of them, and exits
non-zero if any failed. It does not stop at the first failure: finding the next
one on the next run is expensive when the run takes minutes.

Individual gates, when you want one:

```bash
python3 tools/audit_requirements.py                 # catalogue, status, evidence
python3 tools/audit_requirements.py --milestone M0  # one milestone
python3 tools/check_frozen.py --base main           # frozen files: hash + branch diff
python3 tools/check_scope.py --base main            # one active milestone
python3 tools/check_architecture.py                 # layer DAG, namespaces, link edges
python3 tools/check_claims.py                       # unsupported numbers, banned phrasing
python3 tools/check_adrs.py --base main             # decision records and their linkage
python3 tools/check_docs.py                         # required docs, links, evidence markers
python3 tools/scan_secrets.py --staged --history    # worktree, index, history
python3 tools/check_sample_data.py                  # sample size, provenance, external pins
python3 tools/check_pack_manifest.py                # scaffold integrity
```

## Building and testing

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan && ctest --preset asan-ubsan

python3 tools/run_test_layers.py --python .venv/bin/python
bash scripts/check_cpp_style.sh
```

Run `asan-ubsan` before believing a C++ change. It has already caught a
use-after-free that debug and release both accepted.

`run_test_layers.py` fails a layer that collects zero tests. That is deliberate:
a green empty layer is a stub presented as completion.

## Determinism

```bash
python3 tools/determinism_check.py --runs 2 --seed 42 \
    --write-evidence experiments/evidence/AEGIS-005

# The negative fixture must fail; if it stops failing, the harness is broken.
python3 tools/determinism_check.py --producer nondeterministic --expect-failure
```

At M0 this shows that the harness detects nondeterminism. It is not a claim that
AEGIS is deterministic — see [docs/LIMITATIONS.md](LIMITATIONS.md).

## Recording a requirement's status

Never edit `requirements/implementation_status.json` by hand; the writer
validates what the auditor checks, so bypassing it lets an unsupportable claim
into the file.

```bash
python3 tools/update_status.py AEGIS-231 implemented \
    --implementation python/common/config.py \
    --implementation cpp/common/config.cpp \
    --test tests/unit/test_config_validation.py \
    --note "Both loaders validate against configs/schemas/config.v1.json."
```

`verified` additionally requires an independent review record:

```bash
python3 tools/update_status.py AEGIS-231 verified \
    --auditor aegis-spec-auditor --audit-commit <sha> --audit-date 2026-08-06
```

It will refuse if a path does not exist, if no evidence file says anything, or if
the requirement has an open verification obligation.

Registering an obligation, for a requirement that is genuinely implemented but
cannot meet its frozen acceptance yet:

```bash
python3 tools/update_status.py AEGIS-237 implemented \
    --implementation python/common/recovery.py \
    --test tests/unit/test_recovery_contract.py \
    --blocked-until M1 \
    --residual "Contract and interface only; no exchange state exists to recover."

python3 tools/generate_deferred_register.py   # regenerate docs/DEFERRED_VERIFICATION.md
python3 tools/generate_traceability.py        # regenerate docs/TRACEABILITY_MATRIX.md
```

Both generated documents are drift-checked in CI, so regenerate them in the same
commit as the status change.

## Starting a milestone

1. Branch: `git switch -c milestone/mN-name`.
2. Update `docs/BUILD_STATE.md`: active milestone, branch, blockers.
3. Confirm the scope guard agrees: `python3 tools/check_scope.py --base main`.
4. Plan in plan mode, mapped to requirement IDs, before writing code.

## Closing a milestone

1. `bash scripts/ci_local.sh` — every stage green.
2. `python3 tools/audit_requirements.py --milestone MN`.
3. `python3 tools/audit_requirements.py --check-deferred MN` — obligations that
   came due at this milestone must be paid.
4. Write `experiments/milestone-reports/MN.md` covering everything
   [docs/ACCEPTANCE_GATES.md](ACCEPTANCE_GATES.md) requires, including known
   limitations and an explicit statement that no later-milestone feature was
   marked complete.
5. Request an independent `aegis-spec-auditor` review.
6. Promote to `verified` only what the auditor confirms and only where no
   obligation is open.

## When a gate fails

| Symptom | Likely cause | Action |
|---|---|---|
| `frozen file content changed` | A frozen document was edited | Restore it. Only the owner may change it, recorded in BUILD_STATE. |
| `frozen path modified on this branch` | Edited then re-frozen | Same. The hash matching is not sufficient; the diff is checked too. |
| `belongs to a later milestone` | Scope leakage | Move the work to its milestone, or record an owner approval in BUILD_STATE. |
| `no layer claims this file` | New package, undeclared | Add it to `configs/architecture_rules.yaml` with its allowed edges. |
| `would pass vacuously` | A layer is empty at or past its declared milestone | Populate it, or correct `expect_sources_from_milestone`. |
| `numeric claim without resolvable evidence` | A number in prose with no artifact | Point the sentence at a real artifact with an `evidence:` marker, or remove the number. |
| `no tests collected` | A test layer is empty | Populate it. Do not delete the layer. |
| `NEGATIVE GATE BROKEN` | A checker accepted its counter-example | Fix the checker. The fixture is correct by construction. |
| `DRIFT: the environment no longer matches` | Local packages diverged from the lock | `.venv/bin/pip install --require-hashes -r requirements/requirements.lock` |

## Changing a dependency

See [docs/ENVIRONMENT.md](ENVIRONMENT.md#dependency-changes). The order matters:
probe first, because "it installed on my machine" is a different claim from "it
installs on the interpreter CI uses".

## Changing the wire format or a schema

1. Write an ADR. A wire-format change invalidates every recording made under the
   old format, so the decision belongs in the record.
2. Bump the version; never redefine an existing one.
3. Add the new version alongside the old so a migration can be gradual.
4. Update the golden fixture **in the same commit as the encoder change** — that
   is what keeps the two implementations honest.

## Emergency procedures

**A secret was committed.** `tools/scan_secrets.py --history` finds it. Rotate
the credential first — history rewriting does not un-publish anything already
pushed — then rewrite history and force-push with the owner's agreement.

**A frozen file was changed by mistake.** `git checkout <base> -- <path>` and
re-run `tools/check_frozen.py`. Do not re-freeze the hash: that is the thing the
branch-diff check exists to catch.

**A milestone closed with a false claim.** Set the requirement back with
`tools/update_status.py`, record what happened in the milestone report, and add
the check that would have caught it. A gate added after the fact is the only part
of the incident that has lasting value.
