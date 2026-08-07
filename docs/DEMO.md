# Demo

**AEGIS has a deterministic single-instrument exchange core (M1): sequencer,
central limit order book, FIFO matching, snapshot/restore and a replay CLI.
There is still no strategy, no risk engine, no OMS, no portfolio, no research
and no analytics.** M0 built the governance and engineering foundation this
core is checked against; the capability demo the project's definition of done
describes belongs to M9.

Every command below runs against the committed repository and takes a few
minutes end to end.

## 0. Setup

```bash
python3 -m venv .venv
.venv/bin/pip install --require-hashes -r requirements/requirements.lock
export PATH="$PWD/.venv/bin:$PATH"
bash scripts/check_environment.sh
```

The last command checks tool *versions*, not merely presence, and dry-run
installs the lockfile to prove the environment has not drifted from it.

## 1. The specification cannot be edited unnoticed

```bash
python3 tools/check_frozen.py --base main
```

Now try to change one, in a scratch copy so the real file is untouched:

```bash
cp -r docs requirements /tmp/aegis-demo/
echo "sneaky addition" >> /tmp/aegis-demo/docs/MASTER_SPEC.md
python3 tools/check_frozen.py --root /tmp/aegis-demo --no-history   # exits 2
```

The checker reports both the expected and the actual digest. Editing the file
*and* re-freezing its hash does not pass either: the branch diff is checked
independently.

## 2. A completion claim needs evidence that says something

```bash
python3 tools/audit_requirements.py --milestone M0
```

Then try to record an unsupportable claim:

```bash
python3 tools/update_status.py AEGIS-231 verified \
    --implementation python/common/config.py \
    --test tests/unit/test_config_validation.py
# ERROR: 'verified' requires --auditor, --audit-commit and --audit-date
```

A directory, an empty file, a `.gitkeep` or a file containing only TODOs is
likewise rejected as evidence — see `tests/unit/test_evidence_rules.py`.

## 3. The architecture is enforced, not described

```bash
python3 tools/check_architecture.py
```

The committed counter-example tree contains one instance of every rule — a
strategy including the exchange book, a strategy holding a gateway, a
parent-relative include, Python headers outside the bindings layer, a namespace
violation, a mutable global in a deterministic core, an inverted Python import,
and a CMake link edge no include would justify:

```bash
python3 tools/check_architecture.py \
    --root tests/unit/fixtures/arch_violation \
    --rules tests/unit/fixtures/arch_violation/configs/architecture_rules.yaml
# exits 2, reporting all eight
```

## 4. The build, across three presets

```bash
cmake --preset debug     && cmake --build --preset debug     && ctest --preset debug
cmake --preset release   && cmake --build --preset release   && ctest --preset release
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan && ctest --preset asan-ubsan
```

The sanitizer preset is not decoration: it caught a use-after-free in a test that
debug and release both accepted.

## 5. Clock domains cannot be mixed

`tests/cpp/unit/test_time.cpp` asserts this at compile time, because a runtime
test cannot express "this must not compile":

```cpp
static_assert(!Subtractable<AckTime, MonotonicTime>,
              "latency must never be derived by mixing a wall-clock stamp with a monotonic reading");
```

Python cannot reject it at compile time, so it refuses at runtime with an error
naming both domains:

```bash
python3 -c "
import sys; sys.path.insert(0, 'python')
from common.clock import AckTime, MonotonicTime
AckTime(10) - MonotonicTime(5)
"
# TypeError: cannot subtract MonotonicTime (monotonic domain) from AckTime (ack domain) ...
```

## 6. Invalid configuration fails with a message naming the field

```bash
python3 -c "
import sys; sys.path.insert(0, 'python')
from pathlib import Path
from common.config import resolve
resolve(Path('.'), path=Path('tests/unit/fixtures/configs/invalid/future_config_version.json'), environ={})
"
```

Ten invalid fixtures are driven through **both** loaders — Python and C++ — with
a recorded expectation for the field each rejection must mention.

## 7. Determinism, and the harness that would notice its absence

```bash
python3 tools/determinism_check.py --runs 2 --seed 42
python3 tools/determinism_check.py --producer nondeterministic --expect-failure
```

Each run is a separate process with a different `PYTHONHASHSEED`. The second
command is the important one: without a fixture the harness must flag, a green
result would be indistinguishable from a broken harness.

## 8. Two implementations of one wire format, compared live

```bash
python3 -m pytest tests/integration/test_bindings_roundtrip.py -q
```

The C++ encoder runs through the compiled pybind11 module and its bytes are
compared to the Python encoder's across five cases, including the 64-bit
boundaries, a pre-epoch timestamp and non-ASCII identifiers. The test also
asserts the loaded module is the compiled extension — a silent pure-Python
fallback is the usual way a bindings test becomes theatre.

## 9. Test layers, each reported separately

```bash
python3 tools/run_test_layers.py --python .venv/bin/python
```

Seven layers: pytest unit, integration, property, replay and research, plus
ctest unit and property. **A layer that collects zero tests fails.**

## 10. The whole matrix

```bash
bash scripts/ci_local.sh
```

The same stages `.github/workflows/ci.yml` runs, reported as a table.

## 11. A limit order book, matched deterministically

```bash
cmake --build --preset debug
./build/debug/cpp/exchange/app/aegis_exchange_replay \
    --input tests/unit/fixtures/exchange/replay/basic_session.jsonl
```

Ten commands — new orders, a priority-retaining decrease, a cancel-replace, a
cancel, and a market order against an empty book — produce eighteen canonical
event lines: accepts, a trade, terminations, a modify and a replace. Run it
twice; the output is byte-identical.

## 12. Snapshot, restore, and continuation equality

```bash
./build/debug/cpp/exchange/app/aegis_exchange_replay \
    --input tests/unit/fixtures/exchange/replay/basic_session.jsonl \
    --limit 6 --snapshot-out /tmp/aegis-demo-snapshot.bin

./build/debug/cpp/exchange/app/aegis_exchange_replay \
    --input tests/unit/fixtures/exchange/replay/basic_session.jsonl \
    --skip 6 --restore-from /tmp/aegis-demo-snapshot.bin
```

The second command's output is byte-identical to the last nine lines of step
11's uninterrupted run — including every `EventSequence` and `OrderId`
assigned after the split (ADR-0013).

## 13. The exchange engine is deterministic across processes

```bash
python3 tools/determinism_check.py --producer exchange --runs 2
```

Two fresh processes, different `PYTHONHASHSEED`, running the real matching
engine — not a platform stub. This is the AEGIS-005 discharge for the
exchange core.

## 14. Benchmark workloads, with their claim boundary

```bash
cmake --build --preset release
python3 tools/run_exchange_bench.py --preset release
cat experiments/evidence/AEGIS-036/lookup_cancel.json
```

Every field `docs/BENCHMARK_POLICY.md` requires is in the artifact, including
`"local_non_comparable": true` and `environment.virtualisation`. No latency
or throughput number here is a performance claim — see
[docs/LIMITATIONS.md](LIMITATIONS.md).

## What this demo deliberately does not show

No strategy signal, no risk decision, no P&L, no backtest, no attribution, no
Decision Arena scenario, no dashboard, and no latency or throughput figure
that is a comparable claim rather than a disclosed, WSL2-labelled local
measurement. Those are M2 through M9. See
[docs/LIMITATIONS.md](LIMITATIONS.md) and
[docs/DEFERRED_VERIFICATION.md](DEFERRED_VERIFICATION.md).
