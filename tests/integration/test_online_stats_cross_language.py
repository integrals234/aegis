"""AEGIS-107: the compiled C++ estimators and the Python modules agree.

**What this file proves, precisely.** It compares `cpp/statistics` (through
the compiled `aegis_bindings` extension) against
`python/common/online_stats.py`. That module mirrors the C++ recursion step
for step, so agreement here demonstrates that the binding layer transports
values faithfully and that the two transliterations have not drifted -- it
is NOT an independent check of the recursion itself.

The genuinely independent numerical check lives in
`tests/unit/test_offline_stats.py` and in
`tools/generate_stats_evidence.py`, both of which use
`python/common/offline_stats.py` -- textbook definitions computed with
deliberately different algorithms. AEGIS-107's numerical claim rests on
those. An earlier version of this docstring called the two implementations
here "independent"; the M3 closure audit found that false, and it is
retracted.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
from common.online_stats import (
    DrawdownTracker,
    ExponentialStats,
    RollingBeta,
    RollingCovariance,
    RollingMoments,
    RollingRealizedVolatility,
    RollingZScore,
)

pytestmark = pytest.mark.integration

ROOT = Path(__file__).resolve().parents[2]
BUILD_HINT = (
    "The compiled extension was not found. Build it with:\n"
    "    cmake --preset debug && cmake --build --preset debug\n"
    "This test fails rather than skips: a skipped test is not evidence (AEGIS-003)."
)
PRESET_PREFERENCE = ("debug", "release")

TOLERANCE = 1e-9

VALUES = [1.0, 3.0, -2.0, 5.5, 0.0, 4.0, -1.5, 2.5, 3.5, -0.5, 6.0, 1.0]
XS = [1.0, 2.0, 3.0, 2.5, 4.0, 3.0, 5.0, 4.5, 6.0, 5.5]
YS = [2.0, 3.5, 3.0, 5.0, 4.0, 7.0, 6.0, 8.0, 7.5, 9.0]
RETURNS = [0.01, -0.02, 0.015, -0.005, 0.03, -0.01, 0.02, -0.015, 0.005, 0.01]
BENCHMARK_RETURNS = [0.008, -0.018, 0.012, -0.004, 0.025, -0.009, 0.017, -0.012, 0.004, 0.009]
WINDOW = 4
ALPHA = 0.3


def _load_bindings():
    candidates = [ROOT / f"build/{preset}/cpp/bindings" for preset in PRESET_PREFERENCE]
    candidates += [p for p in sorted(ROOT.glob("build/*/cpp/bindings")) if p not in candidates]
    for directory in candidates:
        if any(directory.glob("aegis_bindings*.so")):
            sys.path.insert(0, str(directory))
            break
    try:
        import aegis_bindings
    except ImportError as exc:  # pragma: no cover - exercised when the build is absent
        pytest.fail(f"{BUILD_HINT}\n\nUnderlying import error: {exc}")
    return aegis_bindings


@pytest.fixture(scope="module")
def bindings():
    return _load_bindings()


def test_rolling_moments_agrees_with_python_reference(bindings):
    batch = bindings.rolling_moments_batch(VALUES, WINDOW)
    reference = RollingMoments(WINDOW)
    for i, value in enumerate(VALUES):
        reference.push(value)
        assert batch["means"][i] == pytest.approx(reference.mean(), abs=TOLERANCE)
        assert batch["variances"][i] == pytest.approx(reference.variance(), abs=TOLERANCE)
        assert batch["stddevs"][i] == pytest.approx(reference.stddev(), abs=TOLERANCE)


def test_rolling_covariance_and_correlation_agree_with_python_reference(bindings):
    batch = bindings.rolling_covariance_batch(XS, YS, WINDOW)
    reference = RollingCovariance(WINDOW)
    for i in range(len(XS)):
        reference.push(XS[i], YS[i])
        assert batch["covariances"][i] == pytest.approx(reference.covariance(), abs=TOLERANCE)
        assert batch["correlations"][i] == pytest.approx(reference.correlation(), abs=TOLERANCE)


def test_rolling_zscore_agrees_with_python_reference(bindings):
    scores = bindings.rolling_zscore_batch(VALUES, WINDOW)
    reference = RollingZScore(WINDOW)
    for i, value in enumerate(VALUES):
        expected = reference.push_and_score(value)
        assert scores[i] == pytest.approx(expected, abs=TOLERANCE)


def test_exponential_stats_agrees_with_python_reference(bindings):
    batch = bindings.exponential_stats_batch(VALUES, ALPHA)
    reference = ExponentialStats(ALPHA)
    for i, value in enumerate(VALUES):
        reference.push(value)
        assert batch["means"][i] == pytest.approx(reference.mean(), abs=TOLERANCE)
        assert batch["variances"][i] == pytest.approx(reference.variance(), abs=TOLERANCE)


def test_realized_volatility_agrees_with_python_reference(bindings):
    values = bindings.realized_volatility_batch(RETURNS, WINDOW, 252.0)
    reference = RollingRealizedVolatility(WINDOW)
    for i, r in enumerate(RETURNS):
        reference.push(r)
        assert values[i] == pytest.approx(reference.realized_volatility(252.0), abs=TOLERANCE)


def test_rolling_beta_agrees_with_python_reference(bindings):
    values = bindings.rolling_beta_batch(RETURNS, BENCHMARK_RETURNS, WINDOW)
    reference = RollingBeta(WINDOW)
    for i in range(len(RETURNS)):
        reference.push(RETURNS[i], BENCHMARK_RETURNS[i])
        assert values[i] == pytest.approx(reference.beta(), abs=TOLERANCE)


def test_drawdown_tracker_agrees_with_python_reference(bindings):
    cumulative = [100.0, 120.0, 90.0, 150.0, 80.0, 200.0, 60.0]
    batch = bindings.drawdown_tracker_batch(cumulative)
    reference = DrawdownTracker()
    for i, value in enumerate(cumulative):
        reference.push(value)
        assert batch["high_water_marks"][i] == pytest.approx(
            reference.high_water_mark(), abs=TOLERANCE
        )
        assert batch["current_drawdowns"][i] == pytest.approx(
            reference.current_drawdown(), abs=TOLERANCE
        )
        assert batch["max_drawdowns"][i] == pytest.approx(reference.max_drawdown(), abs=TOLERANCE)
        assert batch["means"][i] == pytest.approx(reference.mean(), abs=TOLERANCE)
        assert batch["variances"][i] == pytest.approx(reference.variance(), abs=TOLERANCE)
