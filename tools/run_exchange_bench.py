#!/usr/bin/env python3
"""Run the M1 exchange benchmark workloads and write disclosed evidence.

`docs/BENCHMARK_POLICY.md` requires every benchmark artifact to carry CPU,
RAM, OS/kernel, virtualisation, compiler, build type, sanitizer/assertion
state, warm-up procedure, message mix, event count, instrument/level/order
counts, fill distribution, allocation count, latency percentiles, throughput
and a reproducible command. `aegis_exchange_bench` (C++) measures the
workload-specific half of that (operation counts, allocation counts,
latencies); this tool adds the machine half via
`tools/capture_environment.py` — already built at M0 for exactly this
purpose — and writes the merged record to
`experiments/evidence/AEGIS-036/` (the order-ID index workload) or
`experiments/evidence/AEGIS-039/` (the output-sensitive matching workloads),
per `experiments/plans/M1.md` §4.7.

**Claim boundary**, restated here because it is the point of this tool
existing rather than a raw binary invocation: every written record carries
`"local_non_comparable": true`. The asserted acceptance is operation and
allocation counts, which are deterministic; timing is recorded because the
policy requires it, not because M1 claims a latency, throughput, HFT or
production figure. See `docs/LIMITATIONS.md`.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from capture_environment import capture

ROOT = Path(__file__).resolve().parents[1]

# (workload name, extra CLI args, requirement ID the evidence is filed under,
# evidence file stem).
MULTI_FILL_K_VALUES = (2, 4, 8, 16, 32)
WORKLOADS: list[tuple[str, list[str], str, str]] = [
    ("lookup_cancel", [], "AEGIS-036", "lookup_cancel"),
    ("single_fill_aggressor", [], "AEGIS-039", "single_fill_aggressor"),
    *[
        (
            "multi_fill_multi_level_aggressor",
            ["--k", str(k)],
            "AEGIS-039",
            f"multi_fill_k{k}",
        )
        for k in MULTI_FILL_K_VALUES
    ],
    ("policy_mix", [], "AEGIS-039", "policy_mix"),
]


def resolve_bench_binary(root: Path, preset: str) -> Path:
    """`AEGIS_EXCHANGE_BENCH` if set, else the named preset's build output.

    A missing binary raises, never skips (mirrors
    `python/common/determinism.py`'s `resolve_exchange_replay_binary`,
    ADR-0012): a skipping benchmark tool is a green stub.
    """
    env_path = os.environ.get("AEGIS_EXCHANGE_BENCH")
    binary = Path(env_path) if env_path else root / "build" / preset / "cpp/exchange/app/aegis_exchange_bench"
    if not binary.exists():
        raise FileNotFoundError(
            f"aegis_exchange_bench not found at {binary}. Build it with "
            f"'cmake --build --preset {preset}', or set AEGIS_EXCHANGE_BENCH to its path."
        )
    return binary


def run_workload(binary: Path, workload: str, extra_args: list[str], seed: int) -> dict[str, Any]:
    result = subprocess.run(
        [str(binary), "--workload", workload, "--seed", str(seed), *extra_args],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"aegis_exchange_bench --workload {workload} failed: {result.stderr}")
    record: dict[str, Any] = json.loads(result.stdout)
    return record


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument(
        "--preset",
        default="release",
        help="CMake preset to bench and to describe in the environment record "
        "(docs/BENCHMARK_POLICY.md: benchmarks may only be quoted from release)",
    )
    parser.add_argument("--seed", type=int, default=1729)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    binary = resolve_bench_binary(root, args.preset)
    environment = capture(root, args.preset)

    written: list[Path] = []
    for workload, extra_args, requirement_id, stem in WORKLOADS:
        record = run_workload(binary, workload, extra_args, args.seed)
        record["environment"] = environment
        record["disclosure_note"] = (
            "docs/BENCHMARK_POLICY.md required fields: message mix, event count, "
            "instrument/level/order counts, fill distribution, allocation count and "
            "latency percentiles are in this record's top level; CPU/RAM/OS/virtualisation/"
            "compiler/build/sanitizer state is in 'environment'."
        )

        out_dir = root / "experiments/evidence" / requirement_id
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / f"{stem}.json"
        out_path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        written.append(out_path)
        print(
            f"{workload:<32} -> {out_path.relative_to(root)}  "
            f"(alloc={record['allocation_count']}, ops={record['measured_operations']})"
        )

    print(f"\nwrote {len(written)} evidence artifacts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
