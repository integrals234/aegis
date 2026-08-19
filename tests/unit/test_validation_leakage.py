"""AEGIS-152, AEGIS-153 -- the look-ahead detector must be falsifiable
against a genuinely LEAKING EXECUTION, not a hand-authored leaky record: it
audits provenance the real estimator emits from its own live state, catches
a seeded leaky estimator's real execution, and passes the real estimator's
honest one."""

from __future__ import annotations

import pytest
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


def test_the_real_estimators_honest_execution_passes_with_zero_violations() -> None:
    records = collect_timing_records_from_real_estimator(_deterministic_series(50), window=20)
    result = audit_feature_timing(records)
    assert result.record_count == 50
    assert result.passed
    assert result.violations == ()


def test_the_seeded_leaky_estimators_real_execution_is_caught() -> None:
    # If the detector passed this fixture, the test itself would be wrong to
    # trust it -- this is the negative-gate proof AEGIS-152 requires. The
    # records here come from run_seeded_leaky_estimator_for_falsifiability_
    # check's OWN buggy execution (it really appends before counting), not
    # from a formula describing what a leak would look like.
    records = run_seeded_leaky_estimator_for_falsifiability_check(_deterministic_series(50), window=20)
    result = audit_feature_timing(records)
    assert not result.passed
    # Every index leaks its own current value into its own fitting window.
    assert len(result.violations) == 50
    assert all("look-ahead" in v.reason for v in result.violations)


def test_violation_records_are_deterministic_and_reference_the_offending_index() -> None:
    records = run_seeded_leaky_estimator_for_falsifiability_check(_deterministic_series(10), window=5)
    result = audit_feature_timing(records)
    first_violation = result.violations[0]
    assert first_violation.feature_index == first_violation.fitting_window_end_index


def test_honest_and_leaky_executions_are_reproducible_and_differ_only_by_the_bug() -> None:
    series = _deterministic_series(30)
    honest_first = collect_timing_records_from_real_estimator(series, window=10)
    honest_second = collect_timing_records_from_real_estimator(series, window=10)
    assert honest_first == honest_second  # Deterministic collection.

    leaky = run_seeded_leaky_estimator_for_falsifiability_check(series, window=10)
    assert len(honest_first) == len(leaky) == 30
    # Same feature indices audited either way; only the fitting window differs.
    assert [r.feature_index for r in honest_first] == [r.feature_index for r in leaky]
    differing = sum(
        1 for h, leak in zip(honest_first, leaky, strict=True)
        if h.fitting_window_end_index != leak.fitting_window_end_index
    )
    assert differing == 30  # Every single record's window shifted by the bug.


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
