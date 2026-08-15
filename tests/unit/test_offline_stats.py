"""The offline reference must be correct by inspection, and must NOT be a
transliteration of the production recursion (ADR-0022; AEGIS-098..107).

Two jobs here. First, pin the reference against values computed by hand from
the definition, so the thing everything else is measured against is itself
anchored. Second, guard the property that makes it a *reference* at all: it
must not drift into reimplementing the incremental algorithm it exists to
check. That second point is not decoration — the M3 closure audit found
exactly that drift in `python/common/online_stats.py`, whose reported
divergence from the C++ was exactly 0.0 everywhere because it *was* the C++
algorithm rewritten in Python.
"""

from __future__ import annotations

import pytest
from common import offline_stats as offline

pytestmark = pytest.mark.unit


# --------------------------------------------------------------- hand-computed


def test_mean_matches_hand_computation():
    assert offline.mean([1.0, 2.0, 3.0, 4.0]) == pytest.approx(2.5)
    assert offline.mean([]) == 0.0


def test_variance_is_sample_ddof_one_not_population():
    # [1,2,3,4]: mean 2.5, squared deviations 2.25+0.25+0.25+2.25 = 5.0.
    # Sample (ddof=1) divides by 3 -> 1.6667; population would give 1.25.
    assert offline.variance([1.0, 2.0, 3.0, 4.0]) == pytest.approx(5.0 / 3.0)
    assert offline.variance([1.0, 2.0, 3.0, 4.0]) != pytest.approx(5.0 / 4.0)


def test_variance_edge_cases_return_zero_not_nan():
    assert offline.variance([]) == 0.0
    assert offline.variance([7.0]) == 0.0
    assert offline.stddev([7.0]) == 0.0


def test_covariance_is_sample_ddof_one():
    xs = [1.0, 2.0, 3.0]
    ys = [2.0, 4.0, 6.0]
    # deviations (-1,0,1) and (-2,0,2): sum of products = 4, / (3-1) = 2.
    assert offline.covariance(xs, ys) == pytest.approx(2.0)


def test_correlation_of_a_perfect_line_is_one():
    xs = [1.0, 2.0, 3.0, 4.0]
    ys = [3.0, 5.0, 7.0, 9.0]
    assert offline.correlation(xs, ys) == pytest.approx(1.0)


def test_correlation_of_a_constant_series_is_zero_not_nan():
    assert offline.correlation([1.0, 1.0, 1.0], [1.0, 2.0, 3.0]) == 0.0


def test_rolling_mean_slides_over_the_window():
    # window 2 over [1,2,3]: [1], [1,2], [2,3] -> 1, 1.5, 2.5
    assert offline.rolling_mean([1.0, 2.0, 3.0], 2) == pytest.approx([1.0, 1.5, 2.5])


def test_rolling_zscore_scores_against_the_prior_window_only():
    """Leakage-free: a value cannot influence its own score."""
    values = [1.0, 2.0, 3.0, 100.0]
    scores = offline.rolling_zscore(values, 3)
    assert scores[0] == 0.0  # No prior window at all.
    # 100.0 is scored against [1,2,3] -- mean 2.0, sample sd 1.0 -- so the
    # score is 98.0 exactly. Had it leaked into its own window the mean would
    # be 26.5 and the score far smaller, so this value pins the convention.
    assert scores[3] == pytest.approx(98.0)


def test_exponential_mean_weights_sum_to_one_on_a_constant_series():
    """A constant series must return that constant at every step, whatever
    the weighting scheme — the sharpest available check that the expanded
    weighted sum normalizes correctly."""
    values = [5.0] * 6
    assert offline.exponential_mean(values, 0.3) == pytest.approx([5.0] * 6)


def test_realized_volatility_is_uncentered_rms():
    returns = [0.1, -0.1]
    # RMS = sqrt((0.01+0.01)/2) = 0.1, unannualized.
    assert offline.rolling_realized_volatility(returns, 2, 1.0)[-1] == pytest.approx(0.1)
    # Annualization multiplies by sqrt(periods).
    assert offline.rolling_realized_volatility(returns, 2, 4.0)[-1] == pytest.approx(0.2)


def test_drawdown_series_tracks_high_water_mark_and_worst_decline():
    pnl = [100.0, 120.0, 90.0, 150.0]
    highs, current, maximum = offline.drawdown_series(pnl)
    assert highs == pytest.approx([100.0, 120.0, 120.0, 150.0])
    assert current == pytest.approx([0.0, 0.0, 30.0, 0.0])
    assert maximum == pytest.approx([0.0, 0.0, 30.0, 30.0])


# ------------------------------------------------- the independence property


def test_offline_reference_shares_no_code_with_the_incremental_module():
    """`offline_stats` must not import or delegate to `online_stats`.

    This is the structural half of the independence property. The numerical
    half — that the two use genuinely different algorithms — cannot be
    asserted mechanically, so it is enforced by review and recorded in
    ADR-0022; what *can* be checked is that the reference never quietly
    starts calling the thing it is supposed to be checking.
    """
    from pathlib import Path

    source = Path(offline.__file__).read_text()
    # Strip the module docstring, which legitimately *names* online_stats in
    # explaining why it is not used.
    body = source.split('"""', 2)[2]
    assert "online_stats" not in body
    assert "from common" not in body
    assert "import common" not in body


def test_offline_variance_is_two_pass_not_updating():
    """A two-pass computation is order-insensitive in a way a running update
    is not: reversing the input must give a bit-identical answer for the
    two-pass form on this fixture. This is a cheap behavioural signature of
    the algorithm actually being two-pass."""
    values = [1e8, 1.0, 2.0, 3.0, 1e-8]
    assert offline.variance(values) == offline.variance(list(reversed(values)))
