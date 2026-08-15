#!/usr/bin/env python3
"""Generate M3 online-statistics evidence (AEGIS-098..106).

Two independent proofs per estimator, both from real production code:

1. The compiled acceptance tests for `cpp/statistics`, executed against the
   real C++ classes (`tools/evidence_gtest.py`), each case recording the
   source file and line of its assertion.
2. Concrete trajectories computed by the real compiled `aegis_bindings`
   extension and by `python/common/offline_stats.py`, with their maximum
   absolute divergence recorded against a stated tolerance.

Point 2 is what makes "matches trusted offline calculations" checkable
rather than asserted: the numbers are in the artifact. The reference is
`offline_stats.py` and NOT `online_stats.py` -- the latter transliterates
the C++ recursion step for step, so its agreement with the C++ is
tautological (it reported exactly 0.0 everywhere, which is what exposed it
during the M3 closure audit). `offline_stats.py` computes each quantity from
its textbook definition with a deliberately different algorithm, so its
agreement is a real cross-check.

**No performance figure is recorded here.** AEGIS-107's own artifact
explains why at length: `docs/BENCHMARK_POLICY.md` (frozen) governs latency
evidence and its disclosure requirements do not describe small numeric
estimators. Nothing in this file is a latency, throughput or comparative
performance claim.

Regenerate with: python3 tools/generate_stats_evidence.py
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from common import offline_stats as offline
from evidence_gtest import merge, run_suite
from evidence_provenance import provenance

EVIDENCE_ROOT = ROOT / "experiments/evidence"
TOLERANCE = 1e-9

# The same committed fixtures the cross-language test and the AEGIS-107
# report use, so the three artifacts describe one comparison rather than
# three subtly different ones.
VALUES = [1.0, 3.0, -2.0, 5.5, 0.0, 4.0, -1.5, 2.5, 3.5, -0.5, 6.0, 1.0]
XS = [1.0, 2.0, 3.0, 2.5, 4.0, 3.0, 5.0, 4.5, 6.0, 5.5]
YS = [2.0, 3.5, 3.0, 5.0, 4.0, 7.0, 6.0, 8.0, 7.5, 9.0]
RETURNS = [0.01, -0.02, 0.015, -0.005, 0.03, -0.01, 0.02, -0.015, 0.005, 0.01]
BENCHMARK_RETURNS = [0.008, -0.018, 0.012, -0.004, 0.025, -0.009, 0.017, -0.012, 0.004, 0.009]
CUMULATIVE_PNL = [100.0, 120.0, 90.0, 150.0, 80.0, 200.0, 60.0]
WINDOW = 4
ALPHA = 0.3
PERIODS_PER_YEAR = 252.0


def load_bindings() -> Any:
    """Import the compiled extension, failing loudly if it is absent.

    Mirrors `tests/integration/test_bindings_roundtrip.py`'s helper: a
    skipped cross-language check is not evidence (AEGIS-003), so a missing
    build is an error here rather than a reason to write a weaker artifact.
    """
    for candidate in sorted((ROOT / "build/debug/cpp/bindings").glob("aegis_bindings*.so")):
        spec = importlib.util.spec_from_file_location("aegis_bindings", candidate)
        if spec is None or spec.loader is None:
            continue
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    raise FileNotFoundError(
        "aegis_bindings extension not found under build/debug/cpp/bindings. "
        "Build it with 'cmake --build --preset debug'."
    )


def _max_abs_diff(left: list[float], right: list[float]) -> float:
    if len(left) != len(right):
        raise ValueError(f"trajectory length mismatch: {len(left)} vs {len(right)}")
    return max((abs(a - b) for a, b in zip(left, right, strict=True)), default=0.0)


def build_comparisons(bindings: Any) -> dict[str, dict[str, float]]:
    """Every divergence between the compiled C++ and the independent reference.

    The reference is `python/common/offline_stats.py`, which computes each
    quantity from its textbook definition with deliberately different
    algorithms (two-pass variance, an expanded weighted sum for the EW mean,
    a plain scan for drawdown). That is what makes a small nonzero divergence
    the *expected* result here: two different-but-correct algorithms differ in
    the last bits. An exact `0.0` across the board would be the suspicious
    outcome, and was in fact the symptom that exposed the earlier
    transliterated reference.
    """
    moments = bindings.rolling_moments_batch(VALUES, WINDOW)
    covariance = bindings.rolling_covariance_batch(XS, YS, WINDOW)
    exponential = bindings.exponential_stats_batch(VALUES, ALPHA)
    drawdown = bindings.drawdown_tracker_batch(CUMULATIVE_PNL)

    ref_high, ref_current, ref_max = offline.drawdown_series(CUMULATIVE_PNL)
    ref_pnl_mean, ref_pnl_var = offline.pnl_moments(CUMULATIVE_PNL)

    return {
        "rolling_mean": {
            "max_abs_diff": _max_abs_diff(moments["means"], offline.rolling_mean(VALUES, WINDOW))
        },
        "rolling_variance": {
            "max_abs_diff": _max_abs_diff(
                moments["variances"], offline.rolling_variance(VALUES, WINDOW)
            )
        },
        "rolling_stddev": {
            "max_abs_diff": _max_abs_diff(
                moments["stddevs"], offline.rolling_stddev(VALUES, WINDOW)
            )
        },
        "rolling_covariance": {
            "max_abs_diff": _max_abs_diff(
                covariance["covariances"], offline.rolling_covariance(XS, YS, WINDOW)
            )
        },
        "rolling_correlation": {
            "max_abs_diff": _max_abs_diff(
                covariance["correlations"], offline.rolling_correlation(XS, YS, WINDOW)
            )
        },
        "rolling_zscore": {
            "max_abs_diff": _max_abs_diff(
                bindings.rolling_zscore_batch(VALUES, WINDOW),
                offline.rolling_zscore(VALUES, WINDOW),
            )
        },
        "exponential_mean": {
            "max_abs_diff": _max_abs_diff(
                exponential["means"], offline.exponential_mean(VALUES, ALPHA)
            )
        },
        "exponential_variance": {
            "max_abs_diff": _max_abs_diff(
                exponential["variances"], offline.exponential_variance(VALUES, ALPHA)
            )
        },
        "realized_volatility": {
            "max_abs_diff": _max_abs_diff(
                bindings.realized_volatility_batch(RETURNS, WINDOW, PERIODS_PER_YEAR),
                offline.rolling_realized_volatility(RETURNS, WINDOW, PERIODS_PER_YEAR),
            )
        },
        "rolling_beta": {
            "max_abs_diff": _max_abs_diff(
                bindings.rolling_beta_batch(RETURNS, BENCHMARK_RETURNS, WINDOW),
                offline.rolling_beta(RETURNS, BENCHMARK_RETURNS, WINDOW),
            )
        },
        "drawdown_high_water_mark": {
            "max_abs_diff": _max_abs_diff(drawdown["high_water_marks"], ref_high)
        },
        "drawdown_current": {
            "max_abs_diff": _max_abs_diff(drawdown["current_drawdowns"], ref_current)
        },
        "drawdown_max": {"max_abs_diff": _max_abs_diff(drawdown["max_drawdowns"], ref_max)},
        "pnl_moment_mean": {"max_abs_diff": _max_abs_diff(drawdown["means"], ref_pnl_mean)},
        "pnl_moment_variance": {
            "max_abs_diff": _max_abs_diff(drawdown["variances"], ref_pnl_var)
        },
    }


SPECS: dict[str, dict[str, Any]] = {
    "AEGIS-098": {
        "title": "Rolling mean",
        "acceptance": "Matches trusted offline calculations.",
        "implementation": ["cpp/statistics/rolling_moments.cpp"],
        "filters": ["RollingMoments.*"],
        "series": ["rolling_mean"],
    },
    "AEGIS-099": {
        "title": "Rolling variance",
        "acceptance": "Matches trusted offline calculations within tolerance.",
        "implementation": ["cpp/statistics/rolling_moments.cpp"],
        "filters": ["RollingMoments.*"],
        "series": ["rolling_variance"],
    },
    "AEGIS-100": {
        "title": "Rolling standard deviation",
        "acceptance": "Edge cases and equivalence tests pass.",
        "implementation": ["cpp/statistics/rolling_moments.cpp"],
        "filters": ["RollingMoments.*"],
        "series": ["rolling_stddev"],
    },
    "AEGIS-101": {
        "title": "Rolling covariance",
        "acceptance": "Matches offline fixture.",
        "implementation": ["cpp/statistics/rolling_covariance.cpp"],
        "filters": ["RollingCovariance.*"],
        "series": ["rolling_covariance"],
    },
    "AEGIS-102": {
        "title": "Rolling correlation",
        "acceptance": "Matches offline fixture and edge cases.",
        "implementation": ["cpp/statistics/rolling_covariance.cpp"],
        "filters": ["RollingCovariance.*"],
        "series": ["rolling_correlation"],
    },
    "AEGIS-103": {
        "title": "Rolling z-score",
        "acceptance": "Timestamped fixture passes.",
        "implementation": ["cpp/statistics/rolling_zscore.cpp"],
        "filters": ["RollingZScore.*"],
        "series": ["rolling_zscore"],
    },
    "AEGIS-104": {
        "title": "Exponential statistics",
        "acceptance": "Numeric fixtures pass.",
        "implementation": ["cpp/statistics/exponential_stats.cpp"],
        "filters": ["ExponentialStats.*"],
        "series": ["exponential_mean", "exponential_variance"],
    },
    "AEGIS-105": {
        "title": "Realized volatility and beta",
        "acceptance": "Offline equivalence tests pass.",
        "implementation": ["cpp/statistics/realized_volatility.cpp"],
        "filters": ["RollingRealizedVolatility.*", "RollingBeta.*"],
        "series": ["realized_volatility", "rolling_beta"],
    },
    "AEGIS-106": {
        "title": "Online drawdown and P&L moments",
        "acceptance": "Scenario tests pass.",
        "implementation": ["cpp/statistics/drawdown_tracker.cpp"],
        "filters": ["DrawdownTracker.*"],
        # Both halves of the requirement: the drawdown series and the P&L
        # moments (mean/variance) the same tracker accumulates.
        "series": [
            "drawdown_high_water_mark",
            "drawdown_current",
            "drawdown_max",
            "pnl_moment_mean",
            "pnl_moment_variance",
        ],
    },
}


def main() -> int:
    stamp = provenance(ROOT)
    bindings = load_bindings()
    comparisons = build_comparisons(bindings)
    all_passed = True

    for requirement, spec in SPECS.items():
        suite = merge(*(run_suite(flt, root=ROOT) for flt in spec["filters"]))
        series = {name: comparisons[name] for name in spec["series"]}
        within = all(entry["max_abs_diff"] <= TOLERANCE for entry in series.values())
        holds = suite["all_passed"] and within
        all_passed = all_passed and holds

        payload = {
            "artifact": f"{requirement}/online_statistics.json",
            "producer": "tools/generate_stats_evidence.py",
            "requirements": [requirement],
            "title": spec["title"],
            "frozen_acceptance": spec["acceptance"],
            "implementation": spec["implementation"],
            "reference_implementation": "python/common/offline_stats.py",
            "acceptance_tests": suite,
            "cross_language_comparison": {
                "tolerance_absolute": TOLERANCE,
                "series": series,
                "within_tolerance": within,
                "method": (
                    "the compiled aegis_bindings extension (real cpp/statistics "
                    "code) is compared against python/common/offline_stats.py, "
                    "which computes each quantity directly from its textbook "
                    "definition using deliberately DIFFERENT algorithms "
                    "(two-pass variance/covariance, the EW mean expanded as a "
                    "weighted sum rather than a recurrence, drawdown by plain "
                    "scan). Both are run over the same committed fixture and the "
                    "maximum absolute difference across the whole trajectory is "
                    "recorded per series. A small nonzero divergence is the "
                    "expected result for two different-but-correct algorithms; "
                    "python/common/online_stats.py is NOT used as the reference "
                    "here because it transliterates the C++ recursion and so "
                    "cannot validate it."
                ),
            },
            "claim": (
                f"{requirement} ({spec['title']}): the frozen acceptance criterion "
                f"“{spec['acceptance']}” is satisfied. "
                f"{suite['test_count']} acceptance test(s) pass against the real C++ "
                "implementation, and the compiled implementation agrees with the "
                "independent Python reference (offline_stats.py, a different "
                "algorithm computed from the definition) to within "
                f"{TOLERANCE} absolute across every recorded series."
            ),
            "not_evidence_for": [
                "Any latency, throughput, memory or comparative performance "
                "property: nothing is timed here and no such claim is made "
                "(docs/BENCHMARK_POLICY.md, docs/CV_CLAIMS_POLICY.md).",
                "Statistical validity of any trading strategy or research result: "
                "these are numeric estimators, not a research finding.",
                "Behaviour on real market data: every fixture is synthetic and "
                "committed in-repository.",
            ],
            "all_claims_hold": holds,
            **stamp,
        }
        directory = EVIDENCE_ROOT / requirement
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "online_statistics.json"
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        print(f"wrote {path.relative_to(ROOT)}")

    if not all_passed:
        print("ERROR: a statistics acceptance test failed or exceeded tolerance",
              file=sys.stderr)
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
