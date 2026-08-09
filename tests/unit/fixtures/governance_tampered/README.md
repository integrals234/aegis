# Counter-example: a branch that tries to authorise itself

A gate that cannot fail proves nothing. This fixture is the committed
counter-example for `tools/governance/authoritative_check.py`, in the same
spirit as `arch_violation` and `claims_bad`.

`trusted/` plays protected `main`. `candidate/` plays a pull request head that
attempts every self-authorisation route R8 was reported for, plus the two the
audit did not name:

| Attempt in `candidate/` | Must be rejected because |
|---|---|
| `docs/BUILD_STATE.md` names `M9` to obtain a tree-wide scope | the active milestone comes from `trusted/`'s policy, and the mirror must agree |
| `docs/BUILD_STATE.md` carries an `- Owner-approved scope changes:` line | nothing reads that line for authority any more |
| `configs/governance/policy.yaml` grants itself an approval | the gate reads `trusted/`'s policy, never the candidate's |
| `configs/milestone_scope.yaml` widens `allowed` to `**` | scope comes from `trusted/`, and the file is a governance path |
| `requirements/frozen_hashes.json` is rewritten to match an edited frozen file | digests are compared against `trusted/`'s manifest |
| `docs/MASTER_SPEC.md` is edited | frozen path, no approval on `main` |

The negative gate runs:

```
python3 tools/governance/authoritative_check.py \
  --trusted-root tests/unit/fixtures/governance_tampered/trusted \
  --candidate-dir tests/unit/fixtures/governance_tampered/candidate \
  --changed-files-from tests/unit/fixtures/governance_tampered/changed_files.txt
```

and **fails if that command succeeds**. `tests/unit/test_authoritative_governance.py`
asserts the same thing, and additionally that each individual attempt is
rejected for its own distinct reason rather than all of them collapsing into
one error that happens to fire.
