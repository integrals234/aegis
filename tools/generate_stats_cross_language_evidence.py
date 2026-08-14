#!/usr/bin/env python3
"""Generate AEGIS-107 evidence: numerical cross-language validation of the
`cpp-statistics` estimators against the independent Python reference.

Every figure here comes from running the actual compiled `aegis_bindings`
extension and the actual `python/common/online_stats.py` reference over the
same committed deterministic fixtures — the same comparison
`tests/integration/test_online_stats_cross_language.py` makes as pytest
assertions, run here to produce a committed artifact instead.

**Performance validation is explicitly out of scope for this artifact.**
AEGIS-107's title names latency and memory alongside numerical output, but
`docs/BENCHMARK_POLICY.md` (frozen) requires CPU model, RAM, OS/kernel,
governor/turbo/affinity state, a specific workload mix and repeated-run
percentile statistics for any latency figure entered as evidence. That
policy is written for the matching-engine benchmarks it already governs
(`cpp/exchange/app/bench_main.cpp`); applying it honestly to seven small
numeric estimators would mean either fabricating a workload mix that does
not describe them or silently relaxing the policy's own disclosure bar for
a different kind of measurement. Neither is acceptable, so this artifact
proves numerical agreement only and states plainly that no latency or
memory figure is recorded here. A future artifact that does can be added as
its own generator, with its own policy-compliant methodology, without
touching this one.

Regenerate with: python3 tools/generate_stats_cross_language_evidence.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from common.online_stats import (
    DrawdownTracker,
    ExponentialStats,
    RollingBeta,
    RollingCovariance,
    RollingMoments,
    RollingRealizedVolatility,
    RollingZScore,
)
from evidence_provenance import provenance

TOLERANCE = 1e-9

VALUES = [1.0, 3.0, -2.0, 5.5, 0.0, 4.0, -1.5, 2.5, 3.5, -0.5, 6.0, 1.0]
XS = [1.0, 2.0, 3.0, 2.5, 4.0, 3.0, 5.0, 4.5, 6.0, 5.5]
YS = [2.0, 3.5, 3.0, 5.0, 4.0, 7.0, 6.0, 8.0, 7.5, 9.0]
RETURNS = [0.01, -0.02, 0.015, -0.005, 0.03, -0.01, 0.02, -0.015, 0.005, 0.01]
BENCHMARK_RETURNS = [0.008, -0.018, 0.012, -0.004, 0.025, -0.009, 0.017, -0.012, 0.004, 0.009]
CUMULATIVE_PNL = [100.0, 120.0, 90.0, 150.0, 80.0, 200.0, 60.0]
WINDOW = 4
ALPHA = 0.3


def _load_bindings() -> Any:
    for preset in ("debug", "release"):
        directory = ROOT / f"build/{preset}/cpp/bindings"
        if any(directory.glob("aegis_bindings*.so")):
            sys.path.insert(0, str(directory))
            break
    else:
        for directory in sorted(ROOT.glob("build/*/cpp/bindings")):
            if any(directory.glob("aegis_bindings*.so")):
                sys.path.insert(0, str(directory))
                break
    import aegis_bindings

    if not getattr(aegis_bindings, "__compiled__", False):
        raise RuntimeError("aegis_bindings did not load as the compiled extension")
    return aegis_bindings


def _max_abs_diff(cpp_values: list[float], python_values: list[float]) -> float:
    return max(abs(c - p) for c, p in zip(cpp_values, python_values, strict=True))


def _compare_rolling_moments(bindings: Any) -> dict[str, Any]:
    batch = bindings.rolling_moments_batch(VALUES, WINDOW)
    reference = RollingMoments(WINDOW)
    means: list[float] = []
    variances: list[float] = []
    for value in VALUES:
        reference.push(value)
        means.append(reference.mean())
        variances.append(reference.variance())
    return {
        "requirement_ids": ["AEGIS-098", "AEGIS-099", "AEGIS-100"],
        "estimator": "RollingMoments",
        "n_observations": len(VALUES),
        "window": WINDOW,
        "max_abs_diff_mean": _max_abs_diff(batch["means"], means),
        "max_abs_diff_variance": _max_abs_diff(batch["variances"], variances),
    }


def _compare_rolling_covariance(bindings: Any) -> dict[str, Any]:
    batch = bindings.rolling_covariance_batch(XS, YS, WINDOW)
    reference = RollingCovariance(WINDOW)
    covariances: list[float] = []
    correlations: list[float] = []
    for x, y in zip(XS, YS, strict=True):
        reference.push(x, y)
        covariances.append(reference.covariance())
        correlations.append(reference.correlation())
    return {
        "requirement_ids": ["AEGIS-101", "AEGIS-102"],
        "estimator": "RollingCovariance",
        "n_observations": len(XS),
        "window": WINDOW,
        "max_abs_diff_covariance": _max_abs_diff(batch["covariances"], covariances),
        "max_abs_diff_correlation": _max_abs_diff(batch["correlations"], correlations),
    }


def _compare_rolling_zscore(bindings: Any) -> dict[str, Any]:
    scores = bindings.rolling_zscore_batch(VALUES, WINDOW)
    reference = RollingZScore(WINDOW)
    expected = [reference.push_and_score(value) for value in VALUES]
    return {
        "requirement_ids": ["AEGIS-103"],
        "estimator": "RollingZScore",
        "n_observations": len(VALUES),
        "window": WINDOW,
        "max_abs_diff_score": _max_abs_diff(list(scores), expected),
    }


def _compare_exponential_stats(bindings: Any) -> dict[str, Any]:
    batch = bindings.exponential_stats_batch(VALUES, ALPHA)
    reference = ExponentialStats(ALPHA)
    means: list[float] = []
    variances: list[float] = []
    for value in VALUES:
        reference.push(value)
        means.append(reference.mean())
        variances.append(reference.variance())
    return {
        "requirement_ids": ["AEGIS-104"],
        "estimator": "ExponentialStats",
        "n_observations": len(VALUES),
        "alpha": ALPHA,
        "max_abs_diff_mean": _max_abs_diff(batch["means"], means),
        "max_abs_diff_variance": _max_abs_diff(batch["variances"], variances),
    }


def _compare_realized_volatility_and_beta(bindings: Any) -> dict[str, Any]:
    vol_values = bindings.realized_volatility_batch(RETURNS, WINDOW, 252.0)
    vol_reference = RollingRealizedVolatility(WINDOW)
    vol_expected = []
    for r in RETURNS:
        vol_reference.push(r)
        vol_expected.append(vol_reference.realized_volatility(252.0))

    beta_values = bindings.rolling_beta_batch(RETURNS, BENCHMARK_RETURNS, WINDOW)
    beta_reference = RollingBeta(WINDOW)
    beta_expected = []
    for asset_r, bench_r in zip(RETURNS, BENCHMARK_RETURNS, strict=True):
        beta_reference.push(asset_r, bench_r)
        beta_expected.append(beta_reference.beta())

    return {
        "requirement_ids": ["AEGIS-105"],
        "estimator": "RollingRealizedVolatility + RollingBeta",
        "n_observations": len(RETURNS),
        "window": WINDOW,
        "periods_per_year": 252.0,
        "max_abs_diff_realized_volatility": _max_abs_diff(list(vol_values), vol_expected),
        "max_abs_diff_beta": _max_abs_diff(list(beta_values), beta_expected),
    }


def _compare_drawdown_tracker(bindings: Any) -> dict[str, Any]:
    batch = bindings.drawdown_tracker_batch(CUMULATIVE_PNL)
    reference = DrawdownTracker()
    high_water_marks: list[float] = []
    current_drawdowns: list[float] = []
    max_drawdowns: list[float] = []
    for value in CUMULATIVE_PNL:
        reference.push(value)
        high_water_marks.append(reference.high_water_mark())
        current_drawdowns.append(reference.current_drawdown())
        max_drawdowns.append(reference.max_drawdown())
    return {
        "requirement_ids": ["AEGIS-106"],
        "estimator": "DrawdownTracker",
        "n_observations": len(CUMULATIVE_PNL),
        "max_abs_diff_high_water_mark": _max_abs_diff(
            batch["high_water_marks"], high_water_marks
        ),
        "max_abs_diff_current_drawdown": _max_abs_diff(
            batch["current_drawdowns"], current_drawdowns
        ),
        "max_abs_diff_max_drawdown": _max_abs_diff(batch["max_drawdowns"], max_drawdowns),
    }


def main() -> int:
    bindings = _load_bindings()

    comparisons = [
        _compare_rolling_moments(bindings),
        _compare_rolling_covariance(bindings),
        _compare_rolling_zscore(bindings),
        _compare_exponential_stats(bindings),
        _compare_realized_volatility_and_beta(bindings),
        _compare_drawdown_tracker(bindings),
    ]

    all_within_tolerance = all(
        value <= TOLERANCE
        for comparison in comparisons
        for key, value in comparison.items()
        if key.startswith("max_abs_diff")
    )
    if not all_within_tolerance:
        raise RuntimeError(
            "a comparison exceeded tolerance -- refusing to write evidence claiming agreement"
        )

    payload = {
        "artifact": "cross_language_validation",
        "requirements": ["AEGIS-107"],
        **provenance(),
        "methodology": (
            "Each estimator is driven through the identical, deterministic, committed "
            "input sequence twice: once through the compiled C++ implementation "
            "(cpp/statistics, via the aegis_bindings *_batch functions) and once through "
            "the independent Python reference (python/common/online_stats.py), which was "
            "written from this project's own reading of the governing mathematics "
            "(ADR-0022), not ported from the C++. Every intermediate value in each "
            "trajectory is compared, not only the final one -- the *_batch bindings "
            "return one output per input precisely so eviction/recursion behaviour mid-"
            "window is checked, not just steady state."
        ),
        "tolerance": {
            "type": "absolute",
            "value": TOLERANCE,
            "rationale": (
                "Both implementations perform the same double-precision arithmetic in "
                "the same operation order (ADR-0022 fixes one numerical convention, "
                "restated independently in each language); the residual is IEEE-754 "
                "rounding noise between two hand-written implementations of the same "
                "recursion, not a methodological difference."
            ),
        },
        "comparisons": comparisons,
        "performance_validation": {
            "status": "not measured in this artifact",
            "reason": (
                "docs/BENCHMARK_POLICY.md (frozen) requires CPU/RAM/OS/governor "
                "disclosure and a specific workload mix for any latency figure entered "
                "as evidence; that policy describes the matching-engine benchmarks it "
                "already governs and does not describe seven small numeric estimators. "
                "Recording a latency number without that disclosure would violate the "
                "policy; inventing a workload mix to satisfy it would misdescribe what "
                "was measured. Neither is acceptable, so no latency or memory figure is "
                "claimed here."
            ),
        },
        "claim": (
            "For every estimator AEGIS-098..106 own, the compiled C++ production "
            "implementation and the independent Python reference agree within "
            f"{TOLERANCE} absolute difference at every point along the committed "
            "deterministic fixture trajectories above."
        ),
        "not_evidence_for": [
            "Any latency or memory comparison for AEGIS-107's 'performance' half -- see "
            "performance_validation above.",
            "AEGIS-098..106's own acceptance criteria, which are discharged by "
            "tests/cpp/unit/test_rolling_*.cpp, test_exponential_stats.cpp, "
            "test_realized_volatility.cpp and test_drawdown_tracker.cpp (C++) and "
            "tests/unit/test_online_stats.py (Python) -- this artifact is cross-language "
            "agreement evidence, not first-party correctness evidence for those IDs.",
        ],
    }

    out_dir = ROOT / "experiments/evidence/AEGIS-107"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "cross_language_validation.json"
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
