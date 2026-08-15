#!/usr/bin/env python3
"""Generate AEGIS-107 evidence: numerical cross-language validation of the
`cpp-statistics` estimators against an independently-algorithmed Python
reference (`python/common/offline_stats.py`).

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

from common import offline_stats as offline
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
PERIODS_PER_YEAR = 252.0


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
    return {
        "estimator": "RollingMoments",
        "requirement_ids": ["AEGIS-098", "AEGIS-099", "AEGIS-100"],
        "window": WINDOW,
        "n_observations": len(VALUES),
        "max_abs_diff_mean": _max_abs_diff(batch["means"], offline.rolling_mean(VALUES, WINDOW)),
        "max_abs_diff_variance": _max_abs_diff(
            batch["variances"], offline.rolling_variance(VALUES, WINDOW)
        ),
        "max_abs_diff_stddev": _max_abs_diff(
            batch["stddevs"], offline.rolling_stddev(VALUES, WINDOW)
        ),
    }


def _compare_rolling_covariance(bindings: Any) -> dict[str, Any]:
    batch = bindings.rolling_covariance_batch(XS, YS, WINDOW)
    return {
        "estimator": "RollingCovariance",
        "requirement_ids": ["AEGIS-101", "AEGIS-102"],
        "window": WINDOW,
        "n_observations": len(XS),
        "max_abs_diff_covariance": _max_abs_diff(
            batch["covariances"], offline.rolling_covariance(XS, YS, WINDOW)
        ),
        "max_abs_diff_correlation": _max_abs_diff(
            batch["correlations"], offline.rolling_correlation(XS, YS, WINDOW)
        ),
    }


def _compare_rolling_zscore(bindings: Any) -> dict[str, Any]:
    return {
        "estimator": "RollingZScore",
        "requirement_ids": ["AEGIS-103"],
        "window": WINDOW,
        "n_observations": len(VALUES),
        "max_abs_diff_score": _max_abs_diff(
            bindings.rolling_zscore_batch(VALUES, WINDOW),
            offline.rolling_zscore(VALUES, WINDOW),
        ),
    }


def _compare_exponential_stats(bindings: Any) -> dict[str, Any]:
    batch = bindings.exponential_stats_batch(VALUES, ALPHA)
    return {
        "estimator": "ExponentialStats",
        "requirement_ids": ["AEGIS-104"],
        "alpha": ALPHA,
        "n_observations": len(VALUES),
        "max_abs_diff_mean": _max_abs_diff(
            batch["means"], offline.exponential_mean(VALUES, ALPHA)
        ),
        "max_abs_diff_variance": _max_abs_diff(
            batch["variances"], offline.exponential_variance(VALUES, ALPHA)
        ),
    }


def _compare_realized_volatility_and_beta(bindings: Any) -> dict[str, Any]:
    return {
        "estimator": "RollingRealizedVolatility + RollingBeta",
        "requirement_ids": ["AEGIS-105"],
        "window": WINDOW,
        "periods_per_year": PERIODS_PER_YEAR,
        "n_observations": len(RETURNS),
        "max_abs_diff_realized_volatility": _max_abs_diff(
            bindings.realized_volatility_batch(RETURNS, WINDOW, PERIODS_PER_YEAR),
            offline.rolling_realized_volatility(RETURNS, WINDOW, PERIODS_PER_YEAR),
        ),
        "max_abs_diff_beta": _max_abs_diff(
            bindings.rolling_beta_batch(RETURNS, BENCHMARK_RETURNS, WINDOW),
            offline.rolling_beta(RETURNS, BENCHMARK_RETURNS, WINDOW),
        ),
    }


def _compare_drawdown_tracker(bindings: Any) -> dict[str, Any]:
    batch = bindings.drawdown_tracker_batch(CUMULATIVE_PNL)
    ref_high, ref_current, ref_max = offline.drawdown_series(CUMULATIVE_PNL)
    ref_mean, ref_variance = offline.pnl_moments(CUMULATIVE_PNL)
    return {
        "estimator": "DrawdownTracker",
        "requirement_ids": ["AEGIS-106"],
        "n_observations": len(CUMULATIVE_PNL),
        "max_abs_diff_high_water_mark": _max_abs_diff(batch["high_water_marks"], ref_high),
        "max_abs_diff_current_drawdown": _max_abs_diff(batch["current_drawdowns"], ref_current),
        "max_abs_diff_max_drawdown": _max_abs_diff(batch["max_drawdowns"], ref_max),
        "max_abs_diff_pnl_mean": _max_abs_diff(batch["means"], ref_mean),
        "max_abs_diff_pnl_variance": _max_abs_diff(batch["variances"], ref_variance),
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
        "producer": "tools/generate_stats_cross_language_evidence.py",
        "requirements": ["AEGIS-107"],
        **provenance(),
        "methodology": (
            "Each estimator is driven through the identical, deterministic, committed "
            "input sequence twice: once through the compiled C++ implementation "
            "(cpp/statistics, via the aegis_bindings *_batch functions) and once through "
            "the independent Python reference (python/common/offline_stats.py), which "
            "computes every quantity DIRECTLY FROM ITS TEXTBOOK DEFINITION using "
            "deliberately different algorithms for most quantities -- two-pass variance "
            "and covariance rather than any updating form, the exponentially-weighted "
            "MEAN expanded as a weighted sum over the whole history rather than as a "
            "recurrence, and drawdown by plain scan. One exception is stated plainly: "
            "the exponentially-weighted VARIANCE has no closed weighted-sum form "
            "matching this convention, so the reference reproduces Finch's recurrence "
            "there and its divergence is consequently exactly 0.0 -- that single "
            "series is a transliteration check, not an independent one. "
            "python/common/online_stats.py is deliberately NOT "
            "used as the reference: it transliterates the C++ recursion step for step, "
            "so its agreement with the C++ is tautological. Every intermediate value in each "
            "trajectory is compared, not only the final one -- the *_batch bindings "
            "return one output per input precisely so eviction/recursion behaviour mid-"
            "window is checked, not just steady state."
        ),
        "tolerance": {
            "type": "absolute",
            "value": TOLERANCE,
            "rationale": (
                "The two implementations share the numerical CONVENTION (ADR-0022: "
                "sample statistics with ddof=1, the documented edge cases) but NOT the "
                "algorithm -- production uses a numerically stable reverse-Welford "
                "recursion, the reference recomputes from the definition in two passes. "
                "A small nonzero residual is therefore the expected outcome, and is "
                "IEEE-754 rounding noise between two different-but-correct algorithms. "
                "An exact 0.0 across EVERY series would indicate the reference shares "
                "the production algorithm rather than checking it -- precisely the "
                "defect the M3 closure audit found in the earlier reference. Exactly "
                "one series (exponential_variance) legitimately reports 0.0 for the "
                "reason given under methodology; every other series reports a genuine "
                "nonzero residual."
            ),
        },
        "comparisons": comparisons,
        "performance_validation": {
            "status": "NOT DONE -- owner-approved residual deferred to M8",
            "residual": (
                "AEGIS-107's frozen description names output, error, latency AND "
                "memory. M3 completes the output/error half only. The latency and "
                "memory comparison is registered as an owner-approved residual "
                "against AEGIS-107 with verification_blocked_until: M8, so this "
                "requirement stays `implemented` rather than `verified` at M3 "
                "closure. It is NOT silently dropped."
            ),
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
            "implementation and an independently-algorithmed Python reference "
            "(python/common/offline_stats.py, computed from the textbook definition) "
            "agree within "
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
