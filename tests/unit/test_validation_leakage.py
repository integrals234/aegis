"""AEGIS-152, AEGIS-153 -- the look-ahead detector must be falsifiable
against a genuinely LEAKING EXECUTION, not a hand-authored leaky record: it
audits provenance the real estimator emits from its own live state, catches
a seeded leaky estimator's real execution, and passes the real estimator's
honest one.

Provenance-integrity ground truth (record.start/end == the actual consumed
window, not a feature-index formula) is covered in
`tests/unit/test_signal_reference.py`, since that is a property of
`research.signal_reference` itself; this file covers the detector and the
honest-vs-leaky execution contrast."""

from __future__ import annotations

import pytest
from research.signal_reference import (
    rolling_zscore_reference,
    rolling_zscore_reference_with_seeded_leak_for_falsifiability_check,
)
from validation.leakage import (
    FeatureTimingRecord,
    audit_feature_timing,
    audit_partition_boundary_consistency,
    collect_timing_records_from_real_estimator,
    run_seeded_leaky_estimator_for_falsifiability_check,
)

pytestmark = pytest.mark.unit


def _deterministic_series(length: int) -> list[float]:
    # A simple deterministic, non-constant series -- its actual values do
    # not matter to either estimator's window-index bookkeeping, only its
    # length does.
    return [float((i * 7 % 13) - 6) for i in range(length)]


# ---- Test A: honest execution -------------------------------------------


def test_the_real_estimators_honest_execution_passes_with_zero_violations() -> None:
    records = collect_timing_records_from_real_estimator(_deterministic_series(50), window=20)
    result = audit_feature_timing(records)
    assert result.record_count == 50
    assert result.passed
    assert result.violations == ()


def test_honest_execution_is_reproducible() -> None:
    series = _deterministic_series(30)
    first = collect_timing_records_from_real_estimator(series, window=10)
    second = collect_timing_records_from_real_estimator(series, window=10)
    assert first == second


# ---- Tests B/C: structural leak enabled, then disabled (ablation) -------


def test_enabling_the_structural_leak_changes_both_numbers_and_provenance_and_is_caught() -> None:
    # Test B. rolling_zscore_reference and its seeded-leak counterpart share
    # one execution engine (research.signal_reference._execute_rolling_
    # zscore) differing only in whether the current observation joins the
    # window before or after being scored. This proves the leak's effect on
    # BOTH the numeric output and the emitted provenance is a consequence of
    # that one shared difference, not two independently hand-authored paths.
    series = _deterministic_series(30)

    honest_scores = list(rolling_zscore_reference(series, window=10))
    leaky_scores = list(rolling_zscore_reference_with_seeded_leak_for_falsifiability_check(series, window=10))
    assert honest_scores != leaky_scores  # The numeric leak is real, not merely a timing artifact.

    honest_records = collect_timing_records_from_real_estimator(series, window=10)
    leaky_records = run_seeded_leaky_estimator_for_falsifiability_check(series, window=10)
    # Same feature indices audited either way; only the consumed window differs.
    assert [r.feature_index for r in honest_records] == [r.feature_index for r in leaky_records]
    for honest, leaky in zip(honest_records, leaky_records, strict=True):
        # The leak makes the window end AT the feature's own index (it just
        # joined before scoring) instead of strictly before it.
        assert leaky.fitting_window_end_index == leaky.feature_index
        assert honest.fitting_window_end_index < honest.feature_index

    leaky_audit = audit_feature_timing(leaky_records)
    assert not leaky_audit.passed
    assert len(leaky_audit.violations) == 30
    assert all("look-ahead" in v.reason for v in leaky_audit.violations)


def test_disabling_the_structural_leak_restores_zero_violations() -> None:
    # Test C (ablation). Same fixture, same provenance machinery, normal
    # (prior-only) update order restored -- violations return to zero. This
    # is the other half of the causality proof: the violation count tracks
    # the update-order flag, not some property baked into the fixture data.
    series = _deterministic_series(30)
    honest_records = collect_timing_records_from_real_estimator(series, window=10)
    assert audit_feature_timing(honest_records).passed


def test_violation_records_are_deterministic_and_reference_the_offending_index() -> None:
    records = run_seeded_leaky_estimator_for_falsifiability_check(_deterministic_series(10), window=5)
    result = audit_feature_timing(records)
    first_violation = result.violations[0]
    assert first_violation.feature_index == first_violation.fitting_window_end_index


# ---- Test E: detector independence ---------------------------------------


def test_the_detector_judges_purely_from_recorded_index_relationships() -> None:
    # AEGIS-107 lesson: FeatureTimingRecord carries no z-score, mean,
    # variance or window-length field, so audit_feature_timing cannot read
    # or reproduce estimator arithmetic even in principle -- only recorded
    # index relationships. Hand-authored records (from no real or leaky
    # execution at all) prove this: the verdict follows purely from index
    # arithmetic, with values no real rolling_zscore_reference call at this
    # window size could have produced.
    honest = (FeatureTimingRecord(feature_index=100, fitting_window_start_index=40, fitting_window_end_index=99),)
    leaking = (FeatureTimingRecord(feature_index=100, fitting_window_start_index=40, fitting_window_end_index=100),)
    assert audit_feature_timing(honest).passed
    assert not audit_feature_timing(leaking).passed


def test_partition_boundary_consistency_flags_training_features_fit_past_the_boundary() -> None:
    # A (hypothetically buggy) record: feature at index 5 (inside train,
    # train_end_index=10) fit using data up to index 15 (past the boundary).
    records = (
        FeatureTimingRecord(feature_index=5, fitting_window_start_index=0, fitting_window_end_index=4),
        FeatureTimingRecord(feature_index=8, fitting_window_start_index=0, fitting_window_end_index=15),
    )
    result = audit_partition_boundary_consistency(records, train_end_index=10)
    assert len(result.violations) == 1
    assert result.violations[0].feature_index == 8


def test_partition_boundary_consistency_passes_when_nothing_crosses_the_boundary() -> None:
    records = collect_timing_records_from_real_estimator(_deterministic_series(30), window=10)
    result = audit_partition_boundary_consistency(records, train_end_index=20)
    assert result.passed
