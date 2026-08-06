#!/usr/bin/env python3
"""Run and report each AEGIS test layer separately (AEGIS-233).

The requirement is "maintain distinct test layers", and the acceptance is "CI
reports each layer". Running one aggregate ``pytest`` satisfies neither: a layer
that exists only as a marker nobody uses stays green forever, and a report that
says "412 passed" cannot tell you the replay layer is empty.

So each layer runs on its own, is reported on its own, and **a layer that
collects zero tests fails**. That last rule is the point of the tool: an empty
layer is a stub presented as completion, which is precisely what AEGIS-003
forbids.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PYTEST_LAYERS = ("unit", "integration", "property", "replay", "research")
CTEST_LAYERS = ("unit", "property")

PYTEST_NO_TESTS_COLLECTED = 5
COUNT = re.compile(r"(\d+) (passed|failed|error)")
CTEST_COUNT = re.compile(r"tests passed, (\d+) tests failed out of (\d+)")


@dataclass
class LayerResult:
    runner: str
    layer: str
    collected: int
    passed: bool
    duration_s: float
    detail: str

    @property
    def label(self) -> str:
        return f"{self.runner}:{self.layer}"


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)


def run_pytest_layer(root: Path, layer: str, python: str) -> LayerResult:
    started = time.monotonic()
    result = run([python, "-m", "pytest", "-m", layer, "-q", "--no-header"], root)
    duration = time.monotonic() - started
    output = result.stdout + result.stderr

    if result.returncode == PYTEST_NO_TESTS_COLLECTED:
        return LayerResult(
            "pytest", layer, 0, False, duration,
            "no tests collected — an empty layer is a stub, not a passing layer",
        )
    counts = {kind: int(n) for n, kind in COUNT.findall(output)}
    collected = sum(counts.values())
    detail = ", ".join(f"{k}={v}" for k, v in sorted(counts.items())) or output.strip()[-200:]
    return LayerResult("pytest", layer, collected, result.returncode == 0, duration, detail)


def run_ctest_layer(root: Path, layer: str, preset: str) -> LayerResult:
    started = time.monotonic()
    result = run(["ctest", "--preset", preset, "-L", f"^{layer}$", "--output-on-failure"], root)
    duration = time.monotonic() - started
    output = result.stdout + result.stderr

    if "No tests were found" in output:
        return LayerResult(
            "ctest", layer, 0, False, duration,
            f"no tests labelled '{layer}' in preset {preset} — an empty layer is a stub",
        )
    match = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", output)
    collected = int(match.group(3)) if match else 0
    detail = match.group(0) if match else output.strip()[-200:]
    return LayerResult("ctest", layer, collected, result.returncode == 0, duration, detail)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--preset", default="debug", help="CMake preset for the C++ layers")
    parser.add_argument("--skip-ctest", action="store_true", help="report Python layers only")
    parser.add_argument("--report", type=Path, help="write a JSON layer report here")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    results = [run_pytest_layer(root, layer, args.python) for layer in PYTEST_LAYERS]
    if not args.skip_ctest:
        results.extend(run_ctest_layer(root, layer, args.preset) for layer in CTEST_LAYERS)

    width = max(len(r.label) for r in results)
    print("\nAEGIS test layers (AEGIS-233)")
    print("-" * (width + 44))
    for result in results:
        status = "PASS" if result.passed else "FAIL"
        print(
            f"{result.label.ljust(width)}  {status}  {result.collected:>4} tests  "
            f"{result.duration_s:6.2f}s  {result.detail}"
        )
    print("-" * (width + 44))

    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps({"report_version": 1, "layers": [asdict(r) for r in results]}, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"wrote {args.report}")

    failed = [r.label for r in results if not r.passed]
    if failed:
        print(f"FAILED layers: {', '.join(failed)}", file=sys.stderr)
        return 2
    print(f"All {len(results)} layers passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
